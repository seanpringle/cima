#include "chat.h"
#include "plan.h"

#include <chrono>
#include <exception>
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

void ChatSession::set_wiki(Wiki* wiki) {
    wiki_ = wiki;
    if (wiki_) {
        tools_.add(make_list_wiki_pages_tool(*wiki_));
        tools_.add(make_read_wiki_page_tool(*wiki_));
        tools_.add(make_write_wiki_page_tool(*wiki_));
        tools_.add(make_edit_wiki_page_tool(*wiki_));
        tools_.add(make_delete_wiki_page_tool(*wiki_));
    }
}

std::string ChatSession::build_notices() {
    // Read current metadata values.
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

    // Collect notices (order: critical before warning, ctx before tc).
    std::string notices;

    // ── Context usage ──
    if (ctx_pct.has_value()) {
        int pct = *ctx_pct;
        std::string tok_info;
        if (est_tok.has_value() && ctx_lim.has_value() && *ctx_lim > 0) {
            tok_info = " (" + std::to_string(*est_tok) + "/" + std::to_string(*ctx_lim) + " tokens)";
        }

        if (pct >= 90) {
            notices += "**\u26A0 Context critical: ~" + std::to_string(pct) +
                "% of context window used" + tok_info +
                "! Archive, prune or summarise session messages before continuing.**\n";
        } else if (pct >= 60) {
            notices += "**\u26A0 Context warning: ~" + std::to_string(pct) +
                "% of context window used" + tok_info +
                ". Consider compacting or pruning droppable messages.**\n";
        }
    }

    // ── Tool call usage ──
    if (tc_used.has_value() && tc_max.has_value() && *tc_max > 0) {
        int pct = *tc_used * 100 / *tc_max;
        std::string tc_info = " (" + std::to_string(*tc_used) + "/" + std::to_string(*tc_max) + ")";

        if (pct >= 90) {
            notices += "**\u26A0 Usage critical: ~" + std::to_string(pct) +
                "% of tool call budget used" + tc_info +
                "! Are you stuck in a loop? Check context usage and prepare a continuation.**\n";
        } else if (pct >= 60) {
            notices += "**\u26A0 Usage warning: ~" + std::to_string(pct) +
                "% of tool call budget used" + tc_info +
                ". Consider whether tools are being used efficiently or schedule a continuation.**\n";
        }
    }

    // Strip trailing newline, if any
    if (!notices.empty() && notices.back() == '\n') {
        notices.pop_back();
    }
    return notices;
}

void ChatSession::restore_last_usage_from_db() {
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

    auto total = read_int("last_total_tokens");
    if (total.has_value()) {
        last_usage_.total_tokens = *total;
    }
    auto prompt = read_int("last_prompt_tokens");
    if (prompt.has_value()) {
        last_usage_.prompt_tokens = *prompt;
    }
    auto completion = read_int("last_completion_tokens");
    if (completion.has_value()) {
        last_usage_.completion_tokens = *completion;
    }
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

        try {
            for (int iter = 0; iter < max_iterations_; iter++) {
            // Refresh session metadata so the agent can see current context usage
            // via SELECT * FROM metadata.
            session_db_.refresh_metadata(model_, context_limit_, last_usage_,
                max_iterations_, iter, cont_slot_.step_count, cont_slot_.max_steps,
                agent_name_);

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
                            Result<std::string> tr;
                            try {
                                tr = tools_.execute(call.name, call.arguments);
                            } catch (const std::exception& e) {
                                tr = std::unexpected(std::string(e.what()));
                            }
                            auto result = tr ? *tr : tr.error();
                            session_db_.add_tool(call.id, result);
                        }
                        // Inject usage notices as a system message (once per iteration)
                        {
                            auto notice = build_notices();
                            if (!notice.empty()) {
                                session_db_.add_system(notice);
                            }
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
                            Result<std::string> tr;
                            try {
                                tr = futures[i].get();
                            } catch (const std::exception& e) {
                                tr = std::unexpected(std::string(e.what()));
                            }
                            auto result = tr ? *tr : tr.error();
                            session_db_.add_tool(calls[i].id, result);
                        }
                        // Inject usage notices as a system message (once per iteration)
                        {
                            auto notice = build_notices();
                            if (!notice.empty()) {
                                session_db_.add_system(notice);
                            }
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
                        Result<std::string> tr;
                        try {
                            tr = tools_.execute(call.name, call.arguments);
                        } catch (const std::exception& e) {
                            tr = std::unexpected(std::string(e.what()));
                        }
                        auto result = tr ? *tr : tr.error();
                        session_db_.add_tool(call.id, result);
                    }
                    // Inject usage notices as a system message (once per iteration)
                    {
                        auto notice = build_notices();
                        if (!notice.empty()) {
                            session_db_.add_system(notice);
                        }
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
        } catch (const std::exception& e) {
            session_db_.truncate_conversation(turn_snapshot);
            cont_slot_.prompt.reset();
            return std::unexpected(std::string(e.what()));
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
