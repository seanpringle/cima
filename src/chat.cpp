#include "chat.h"
#include "plan.h"

#include <future>
#include <mutex>
#include <unordered_map>

ChatSession::ChatSession(Config config, CancellationToken cancelled)
    : model_(std::move(config.model)), reasoning_effort_(std::move(config.reasoning_effort)),
      safe_dir_(std::make_shared<std::string>(std::move(config.safe_dir))),
      api_key_(config.api_key), max_iterations_(config.max_tool_iterations),
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
        config.worktree_base,
        /*include_write=*/true);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(plan_));
    tools_.add(make_read_plan_tool(plan_));
    tools_.add(make_comment_plan_tool(plan_));

    // Each session gets an in-memory SQLite database tool.
    // The conversation lives in 'messages' and 'tool_calls' tables
    // within this DB, so the agent can read/write its own context.
    tools_.add(make_query_session_tool(session_db_));
}

void ChatSession::clear() { session_db_.clear_conversation(); }

void ChatSession::compact() { session_db_.prune_droppable(); }

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
                context_limit_discovered_ = true;
            }
        }
        if (!context_limit_discovered_) {
            int discovered = client_.fetch_model_context_limit(model_);
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (discovered > 0)
                cache[cache_key] = discovered;
            context_limit_discovered_ = true;
        }
    }

    auto snapshot = session_db_.message_count();
    session_db_.add_user(user_input);

    for (int iter = 0; iter < max_iterations_; iter++) {
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
            session_db_.truncate_conversation(snapshot);
            auto msg = stream_result.error();
            auto raw = client_.last_raw_response();
            if (!raw.empty()) {
                msg += " | raw: " + raw.substr(0, 500);
            }
            return std::unexpected(std::move(msg));
        }

        if (stream_errored && content.empty()) {
            session_db_.truncate_conversation(snapshot);
            return std::unexpected(stream_error);
        }

        auto calls = tool_acc.finalize();
        if (!calls.empty()) {
            session_db_.add_assistant("", reasoning, calls);

            if (*cancelled_) {
                session_db_.truncate_conversation(snapshot);
                return std::unexpected("Interrupted during tool execution");
            }

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
                            session_db_.truncate_conversation(snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        auto tr = tools_.execute(call.name, call.arguments);
                        session_db_.add_tool(call.id, tr ? *tr : tr.error());
                    }
                } else {
                    // Parallel execution for read-only tools
                    std::vector<std::future<Result<std::string>>> futures;
                    futures.reserve(calls.size());
                    for (size_t i = 0; i < calls.size(); i++) {
                        futures.push_back(std::async(std::launch::async,
                            [&, i] { return tools_.execute(calls[i].name, calls[i].arguments); }));
                    }
                    for (size_t i = 0; i < calls.size(); i++) {
                        if (*cancelled_) {
                            session_db_.truncate_conversation(snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_(
                                "\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        auto tr = futures[i].get();
                        session_db_.add_tool(calls[i].id, tr ? *tr : tr.error());
                    }
                }
            } else {
                for (const auto& call : calls) {
                    if (*cancelled_) {
                        session_db_.truncate_conversation(snapshot);
                        return std::unexpected("Interrupted during tool execution");
                    }
                    if (output_cb_) {
                        output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                            OutputType::ToolInvocation);
                    }
                    auto tr = tools_.execute(call.name, call.arguments);
                    session_db_.add_tool(call.id, tr ? *tr : tr.error());
                }
            }
            continue;
        }

        session_db_.add_assistant(content, reasoning);
        return ChatResult{std::move(content), std::move(reasoning)};
    }

    session_db_.truncate_conversation(snapshot);
    return std::unexpected("Maximum tool call iterations (" + std::to_string(max_iterations_) +
        ") reached. Increase via LLM_MAX_TOOL_ITERATIONS env var.");
}
