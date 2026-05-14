#include "chat.h"
#include "plan.h"

#include <cctype>
#include <iostream>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

ChatSession::ChatSession(Config config, CancellationToken cancelled)
    : model_(std::move(config.model)), reasoning_effort_(std::move(config.reasoning_effort)),
      safe_dir_(std::make_shared<std::string>(std::move(config.safe_dir))),
      api_base_(config.api_base), api_key_(config.api_key), max_iterations_(config.max_tool_iterations),
      context_limit_(config.context_limit),
      system_prompt_(std::move(config.system_prompt)),
      client_(std::move(config.api_base), std::move(config.api_key)),
      cancelled_(cancelled ? std::move(cancelled) : make_cancellation_token()) {
    // Share the cancellation token with tools and client
    tools_.set_cancelled(cancelled_);
    client_.set_cancelled(cancelled_);

    tools_.add_defaults(safe_dir_,
        config.read_only_paths,
        config.search_api_key,
        config.search_engine_id,
        config.search_endpoint,
        /*include_write=*/true);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(plan_));
    tools_.add(make_read_plan_tool(plan_));
    tools_.add(make_comment_plan_tool(plan_));

    // Each session gets an in-memory SQLite database tool.
    // The conversation lives in 'messages' and 'tool_calls' tables
    // within this DB, so the agent can read/write its own context.
    tools_.add(make_query_session_tool(session_db_));

    // Continuation tool — allows the agent to schedule its own next turn
    cont_slot_.max_steps = config.max_continuation_steps;
    cont_slot_.delay_ms = config.continuation_delay_ms;
    tools_.add(make_schedule_continuation_tool(cont_slot_, cancelled_));

    // ── Session DB persistence ──
    if (!config.session_db_path.empty()) {
        auto load_result = session_db_.load_from_file(config.session_db_path);
        if (!load_result) {
            // If load fails (e.g. first run, file doesn't exist), that's OK —
            // we'll start with a fresh DB and save on close.
        }
        session_db_.set_auto_save_path(config.session_db_path);
    }
}

void ChatSession::clear() {
    session_db_.clear_conversation();
    session_db_.reset_notices();
}

void ChatSession::compact() {
    session_db_.prune_droppable();
    session_db_.reset_notices();
}

std::string ChatSession::inject_usage_notices(std::string result) {
    // Read current metadata values.
    // Returns empty string for any value that can't be read.
    auto read_int = [&](const std::string& key) -> std::optional<int> {
        auto res = session_db_.execute("SELECT value FROM metadata WHERE key = '" + key + "'");
        if (!res) return std::nullopt;
        auto arr = json::parse(*res, nullptr, false);
        if (!arr.is_array() || arr.empty()) return std::nullopt;
        auto v = arr[0]["value"];
        if (v.is_null() || !v.is_string()) return std::nullopt;
        try { return std::stoi(v.get<std::string>()); }
        catch (...) { return std::nullopt; }
    };

    auto ctx_pct = read_int("context_usage_percent");
    auto tc_used = read_int("tool_calls_used");
    auto tc_max  = read_int("max_tool_iterations");
    auto est_tok = read_int("estimated_tokens");
    auto ctx_lim = read_int("context_limit");

    // Collect banners to prepend (order: critical before warning, ctx before tc).
    std::string banners;

    // ── Context usage ──
    if (ctx_pct.has_value()) {
        int pct = *ctx_pct;
        std::string tok_info;
        if (est_tok.has_value() && ctx_lim.has_value() && *ctx_lim > 0) {
            tok_info = " (" + std::to_string(*est_tok) + "/" + std::to_string(*ctx_lim) + " tokens)";
        }

        if (pct >= 90 && !session_db_.is_notice_shown("notice_ctx_critical")) {
            banners += "[context critical: ~" + std::to_string(pct) +
                "% of context window used" + tok_info +
                "! Archive, prune or summarise session messages before continuing.]\n\n";
            session_db_.mark_notice_shown("notice_ctx_critical");
        } else if (pct >= 60 && !session_db_.is_notice_shown("notice_ctx_warning")) {
            banners += "[context warning: ~" + std::to_string(pct) +
                "% of context window used" + tok_info +
                ". Consider compacting or pruning droppable messages.]\n\n";
            session_db_.mark_notice_shown("notice_ctx_warning");
        }
    }

    // ── Tool call usage ──
    if (tc_used.has_value() && tc_max.has_value() && *tc_max > 0) {
        int pct = *tc_used * 100 / *tc_max;
        std::string tc_info = " (" + std::to_string(*tc_used) + "/" + std::to_string(*tc_max) + ")";

        if (pct >= 90 && !session_db_.is_notice_shown("notice_tc_critical")) {
            banners += "[usage critical: ~" + std::to_string(pct) +
                "% of tool call budget used" + tc_info +
                "! Are you stuck in a loop? Check context usage and prepare a continuation.]\n\n";
            session_db_.mark_notice_shown("notice_tc_critical");
        } else if (pct >= 60 && !session_db_.is_notice_shown("notice_tc_warning")) {
            banners += "[usage warning: ~" + std::to_string(pct) +
                "% of tool call budget used" + tc_info +
                ". Consider whether tools are being used efficiently or schedule a continuation.]\n\n";
            session_db_.mark_notice_shown("notice_tc_warning");
        }
    }

    if (!banners.empty()) {
        result = banners + result;
    }
    return result;
}

Result<ChatResult> ChatSession::run_once(const std::string& user_input) {
    // ── Discover context limit (once per model/endpoint) ──
    if (!context_limit_discovered_) {
        static std::mutex cache_mutex;
        static std::unordered_map<std::string, int> cache;

        std::string cache_key = client_.url() + ":" + model_;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = cache.find(cache_key);
            if (it != cache.end()) {
                context_limit_ = it->second;
                context_limit_discovered_ = true;
            }
        }
        if (!context_limit_discovered_) {
            int discovered = client_.fetch_model_context_limit(model_);
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (discovered > 0) {
                cache[cache_key] = discovered;
                context_limit_ = discovered;
            }
            context_limit_discovered_ = true;
        }
    }

    // ── Continuation loop ──
    // Each iteration of this loop processes one "turn" (initial user input
    // or a scheduled continuation).  Per-turn snapshots allow errors in one
    // turn to roll back only that turn's messages, preserving prior work.
    std::string last_content;
    std::string last_reasoning;

    // The prompt for the current turn.  Starts with the user's input; on
    // subsequent turns it comes from the continuation slot.
    std::string turn_prompt = user_input;

    while (true) {
        // Snapshot BEFORE adding this turn's prompt.  If anything goes wrong
        // we roll back to here, which removes the prompt + all subsequent
        // messages from this turn, preserving prior conversation history.
        auto turn_snapshot = session_db_.message_count();

        // Add the prompt for this turn (user input or continuation)
        session_db_.add_user(turn_prompt);

        bool produced_content = false;

        for (int iter = 0; iter < max_iterations_; iter++) {
            // Refresh session metadata so the agent can see current context usage
            // via SELECT * FROM metadata.
            session_db_.refresh_metadata(model_, context_limit_, last_usage_,
                max_iterations_, iter, cont_slot_.step_count, cont_slot_.max_steps);

            // Build payload from the session DB (messages + tool_calls tables).
            // The agent may have modified these tables via query_session during
            // previous tool execution to manage its own context.
            json payload = {{"model", model_},
                {"reasoning_effort", reasoning_effort_},
                {"messages", session_db_.build_openai_payload(system_prompt_)},
                {"tools", tools_.to_openai_tools()},
                {"stream", true}};
            payload["stream_options"] = {{"include_usage", true}};
            std::string content;
            std::string reasoning;
            ToolAccumulator tool_acc;
            bool stream_errored = false;
            std::string stream_error;

            auto on_data = [&](const json& data) {
                // Capture token usage if present (may appear in the final chunk).
                auto usage_it = data.find("usage");
                if (usage_it != data.end() && usage_it->is_object()) {
                    try {
                        last_usage_ = usage_it->get<Usage>();
                    } catch (...) {
                        // Ignore malformed usage data.
                    }
                }

                if (!data.contains("choices") || data["choices"].empty())
                    return;
                const auto& delta = data["choices"][0]["delta"];

                auto rc_it = delta.find("reasoning_content");
                if (rc_it != delta.end() && rc_it->is_string()) {
                    auto text = rc_it->get<std::string>();
                    reasoning += text;
                    if (output_cb_)
                        output_cb_(text, OutputType::Reasoning);
                }

                auto tc_it = delta.find("tool_calls");
                if (tc_it != delta.end() && tc_it->is_array()) {
                    tool_acc.apply(delta);
                }

                auto c_it = delta.find("content");
                if (c_it != delta.end() && c_it->is_string()) {
                    auto text = c_it->get<std::string>();
                    content += text;
                    if (output_cb_)
                        output_cb_(text, OutputType::Content);
                }
            };

            SSEParser::Callbacks callbacks({
                .on_data = on_data,
                .on_done = []() {},
                .on_error =
                    [&](const std::string& err) {
                        stream_errored = true;
                        stream_error = err;
                    },
            });

            auto stream_result = client_.stream_chat(payload, callbacks);

            if (!stream_result) {
                // Stream transport error — roll back only this turn
                session_db_.truncate_conversation(turn_snapshot);
                cont_slot_.prompt.reset();
                auto msg = stream_result.error();
                auto raw = client_.last_raw_response();
                if (!raw.empty()) {
                    msg += " | raw: " + raw.substr(0, 500);
                }
                return std::unexpected(std::move(msg));
            }

            if (stream_errored && content.empty()) {
                // Stream error with no content — roll back only this turn
                session_db_.truncate_conversation(turn_snapshot);
                cont_slot_.prompt.reset();
                return std::unexpected(stream_error);
            }

            auto calls = tool_acc.finalize();
            if (!calls.empty()) {
                session_db_.add_assistant("", reasoning, calls);

                if (*cancelled_) {
                    // User cancelled — roll back only this turn
                    session_db_.truncate_conversation(turn_snapshot);
                    cont_slot_.prompt.reset();
                    return std::unexpected("Interrupted during tool execution");
                }

                int remaining = max_iterations_ - iter - 1;

                if (calls.size() > 1) {
                    // If any tool in the batch has Write permission, serialize
                    // the batch to avoid race conditions on shared resources
                    // (e.g. .git/index.lock) that the per-tool mutex can't
                    // fully protect (e.g. git_add + git_commit with all:true).
                    auto write_tools = tools_.tool_names_by_permission(ToolPermission::Write);
                    bool has_write = false;
                    for (const auto& call : calls) {
                        if (write_tools.count(call.name)) {
                            has_write = true;
                            break;
                        }
                    }

                    if (has_write) {
                        // Serial execution for write tools
                        for (const auto& call : calls) {
                            if (*cancelled_) {
                                session_db_.truncate_conversation(turn_snapshot);
                                cont_slot_.prompt.reset();
                                return std::unexpected("Interrupted during tool execution");
                            }
                            if (output_cb_) {
                                output_cb_(
                                    "\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                    OutputType::ToolInvocation);
                            }
                            auto tr = tools_.execute(call.name, call.arguments);
                            auto result = tr ? *tr : tr.error();
                            session_db_.add_tool(call.id, inject_usage_notices(result));
                        }
                    } else {
                        // Parallel execution for read-only tools
                        std::vector<std::future<Result<std::string>>> futures;
                        futures.reserve(calls.size());
                        for (size_t i = 0; i < calls.size(); i++) {
                            futures.push_back(std::async(std::launch::async,
                                [&, i] {
                                    return tools_.execute(calls[i].name, calls[i].arguments);
                                }));
                        }
                        for (size_t i = 0; i < calls.size(); i++) {
                            if (*cancelled_) {
                                session_db_.truncate_conversation(turn_snapshot);
                                cont_slot_.prompt.reset();
                                return std::unexpected("Interrupted during tool execution");
                            }
                            if (output_cb_) {
                                output_cb_(
                                    "\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments +
                                        ")",
                                    OutputType::ToolInvocation);
                            }
                            auto tr = futures[i].get();
                            auto result = tr ? *tr : tr.error();
                            session_db_.add_tool(calls[i].id, inject_usage_notices(result));
                        }
                    }
                } else {
                    for (const auto& call : calls) {
                        if (*cancelled_) {
                            session_db_.truncate_conversation(turn_snapshot);
                            cont_slot_.prompt.reset();
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        auto tr = tools_.execute(call.name, call.arguments);
                        auto result = tr ? *tr : tr.error();
                        session_db_.add_tool(call.id, inject_usage_notices(result));
                    }
                }
                continue;
            }

            // No tool calls — content response.  Remember it but don't return
            // yet — we may have a continuation pending.
            session_db_.add_assistant(content, reasoning);
            last_content = content;
            last_reasoning = reasoning;
            produced_content = true;
            break;
        }

        if (!produced_content) {
            // Budget exhausted — preserve accumulated work so the user/agent
            // can continue (e.g. via schedule_continuation).
            cont_slot_.prompt.reset();
            std::string msg = "Tool call budget exhausted (" +
                std::to_string(max_iterations_) + " iterations). " +
                "Use `schedule_continuation` to continue in a new turn if needed.";
            session_db_.add_assistant(msg, "");
            last_content = msg;
            last_reasoning = "";
            produced_content = true;
        }

        // ── Check for scheduled continuation ──
        if (!cont_slot_.prompt.has_value())
            break; // normal end — no continuation

        // Guard rails
        if (*cancelled_) {
            cont_slot_.prompt.reset();
            break;
        }
        if (cont_slot_.max_steps > 0 && cont_slot_.step_count >= cont_slot_.max_steps) {
            cont_slot_.prompt.reset();
            break;
        }

        // Set up the next turn's prompt from the continuation slot.
        // The prompt will be added (with a fresh snapshot) at the top of
        // the next loop iteration.
        {
            turn_prompt = std::move(*cont_slot_.prompt);
            cont_slot_.prompt.reset();

            // Notify the UI via the output callback
            if (output_cb_)
                output_cb_("Continuing: " + turn_prompt, OutputType::Continuation);

            cont_slot_.step_count++;

            // Anti-DoS delay
            if (cont_slot_.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(cont_slot_.delay_ms));
            }
        }

        // Continue the outer loop to process the new turn
        continue;
    }

    return ChatResult{std::move(last_content), std::move(last_reasoning)};
}

// ===================================================================
// Session title generation (free function + instance method wrapper)
// ===================================================================

static Result<std::string> clean_title_response(const json& j) {
    // Some reasoning models put the entire output in reasoning_content
    // and leave content empty. Fall back to reasoning_content if needed.
    const auto& msg = j["choices"][0]["message"];
    std::string title;
    if (auto it = msg.find("content"); it != msg.end() && it->is_string() && !it->get<std::string>().empty()) {
        title = it->get<std::string>();
    } else if (auto it2 = msg.find("reasoning_content"); it2 != msg.end() && it2->is_string() && !it2->get<std::string>().empty()) {
        title = it2->get<std::string>();
    }

    // Clean up the title: lowercase, replace non-alphanumeric with hyphens
    std::string clean;
    for (char c : title) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!clean.empty() && clean.back() != '-') {
            clean += '-';
        }
    }
    // Trim trailing hyphens
    while (!clean.empty() && clean.back() == '-') {
        clean.pop_back();
    }
    // Limit length
    if (clean.size() > 50) {
        clean = clean.substr(0, 50);
        while (!clean.empty() && clean.back() == '-') {
            clean.pop_back();
        }
    }
    if (clean.empty()) {
        std::cerr << "cima: title generation produced empty title from raw response:\n"
                  << j.dump(2) << std::endl;
        clean = "untitled";
    }
    return clean;
}

Result<std::string> generate_session_title(const std::string& api_base,
    const std::string& api_key,
    const std::string& model,
    const std::vector<std::string>& conversation) {

    // Build messages: include the conversation as context, then ask for a title.
    json msgs = json::array();

    msgs.push_back({{"role", "system"},
        {"content",
            "You are a title generator. Given a conversation between a user and an "
            "AI coding assistant, generate a short session title (3-5 words, lowercase, "
            "use hyphens for spaces, filesystem-safe). Return ONLY the title. "
            "No punctuation, no quotes, no explanation."}});

    // Add conversation context (alternating user/assistant messages).
    for (size_t i = 0; i < conversation.size(); i++) {
        std::string role = (i % 2 == 0) ? "user" : "assistant";
        msgs.push_back({{"role", role}, {"content", conversation[i]}});
    }

    // Final instruction to produce the title.
    msgs.push_back({{"role", "user"},
        {"content",
            "Based on this conversation, generate a short session title "
            "(3-5 words, lowercase, hyphens for spaces, filesystem-safe). "
            "Return ONLY the title."}});

    json payload = {{"model", model},
        {"messages", msgs},
        {"max_tokens", 500},
        {"stream", false}};

    ChatClient temp_client(api_base, api_key);
    auto result = temp_client.chat(payload);
    if (!result) {
        return std::unexpected(result.error());
    }

    try {
        return clean_title_response(*result);
    } catch (const std::exception& e) {
        return std::unexpected("Failed to parse title: " + std::string(e.what()));
    }
}

Result<std::string> ChatSession::generate_session_title(const std::string& prompt) {
    return ::generate_session_title(api_base_, api_key_, model_, {prompt});
}
