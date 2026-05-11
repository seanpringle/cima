#include "chat.h"
#include "plan.h"

#include <future>

ChatSession::ChatSession(Config config, TabType tab_type)
    : model_(std::move(config.model)), reasoning_effort_(std::move(config.reasoning_effort)),
      safe_dir_(std::move(config.safe_dir)),
      api_key_(config.api_key),
      max_iterations_(config.max_tool_iterations),
      context_limit_(static_cast<size_t>(config.context_limit)),
      compact_threshold_(static_cast<size_t>(config.compact_threshold)),
      tab_type_(tab_type),
      conversation_(tab_type == TabType::Planner ? config.planner_prompt : config.builder_prompt),
      client_(std::move(config.api_base), std::move(config.api_key)) {
    tools_.add_defaults(safe_dir_, config.read_only_paths, config.search_api_key,
        config.search_engine_id, config.search_endpoint,
        /*include_write=*/tab_type_ != TabType::Planner);

    // Planner gets all three plan tools; Builder gets read-only plan tools
    if (tab_type_ == TabType::Planner) {
        tools_.add(make_write_plan_tool());
        tools_.add(make_read_plan_tool());
        tools_.add(make_comment_plan_tool());
    } else {
        tools_.add(make_read_plan_tool());
        tools_.add(make_comment_plan_tool());
    }

    // Wire up the summary callback for compaction
    conversation_.set_summary_callback(
        [this](const std::vector<Message>& msgs, size_t max_tokens) {
            return summarize_messages_(msgs, max_tokens);
        });

    // Try to discover context limit from the API. Only use it if the user
    // didn't explicitly set LLM_CONTEXT_LIMIT (config.context_limit still
    // has the default 300000 if unset — we can't distinguish that, but the
    // env var takes precedence because we only override if discovery succeeds
    // and we treat config as the baseline). If the user set the env var to
    // something, config.context_limit will already reflect it. Discovery
    // only fills in if config.context_limit is still the hardcoded default.
    if (config.context_limit == 300000) {
        int discovered = client_.fetch_model_context_limit(model_);
        if (discovered > 0) {
            context_limit_ = static_cast<size_t>(discovered);
        }
    }
}

void ChatSession::clear() { conversation_.clear(); }

void ChatSession::compact() {
    conversation_.compact();
}

Result<ChatResult> ChatSession::run_once(const std::string& user_input) {
    auto snapshot = conversation_.size();
    conversation_.add_user(user_input);

    for (int iter = 0; iter < max_iterations_; iter++) {
        // ── Compact if needed before building the API payload ──
        if (conversation_.needs_compaction(context_limit_, compact_threshold_)) {
            conversation_.compact();
            if (output_cb_) {
                output_cb_("[\u2302 compaction]", OutputType::ToolInvocation);
            }
        }

        json payload = {{"model", model_},
            {"reasoning_effort", reasoning_effort_},
            {"messages", conversation_.to_openai_messages()},
            {"tools", tools_.to_openai_tools()},
            {"stream", true}};

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
            conversation_.truncate(snapshot);
            auto msg = stream_result.error();
            auto raw = client_.last_raw_response();
            if (!raw.empty()) {
                msg += " | raw: " + raw.substr(0, 500);
            }
            return std::unexpected(std::move(msg));
        }

        if (stream_errored && content.empty()) {
            conversation_.truncate(snapshot);
            return std::unexpected(stream_error);
        }

        auto calls = tool_acc.finalize();
        if (!calls.empty()) {
            conversation_.add_assistant("", reasoning, calls);

            if (g_interrupted) {
                conversation_.truncate(snapshot);
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
                        if (g_interrupted) {
                            conversation_.truncate(snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        auto tr = tools_.execute(call.name, call.arguments);
                        conversation_.add_tool(call.id, tr ? *tr : tr.error());
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
                        if (g_interrupted) {
                            conversation_.truncate(snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_("\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        auto tr = futures[i].get();
                        conversation_.add_tool(calls[i].id, tr ? *tr : tr.error());
                    }
                }
            } else {
                for (const auto& call : calls) {
                    if (g_interrupted) {
                        conversation_.truncate(snapshot);
                        return std::unexpected("Interrupted during tool execution");
                    }
                    if (output_cb_) {
                        output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                            OutputType::ToolInvocation);
                    }
                    auto tr = tools_.execute(call.name, call.arguments);
                    conversation_.add_tool(call.id, tr ? *tr : tr.error());
                }
            }
            continue;
        }

        conversation_.add_assistant(content, reasoning);
        return ChatResult{std::move(content), std::move(reasoning)};
    }

    conversation_.truncate(snapshot);
    return std::unexpected(
        "Maximum tool call iterations (" + std::to_string(max_iterations_) +
            ") reached. Increase via LLM_MAX_TOOL_ITERATIONS env var.");
}

std::optional<std::string> ChatSession::summarize_messages_(
    const std::vector<Message>& msgs, size_t max_tokens) {
    // Build a compact conversation asking the LLM to summarize old exchanges.
    Conversation summary_conv(
        "Summarize the following conversation exchanges concisely. "
        "Preserve the user's intent, key decisions, and any information "
        "that will be needed to continue the task. "
        "Output only the summary, no preamble.");

    for (const auto& msg : msgs) {
        if (msg.role == "user" && msg.content) {
            summary_conv.add_user(*msg.content);
        } else if (msg.role == "assistant" && msg.content) {
            summary_conv.add_assistant(*msg.content);
        } else if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            // Tool-call-only assistant messages: just note what was called
            std::string summary;
            for (const auto& tc : msg.tool_calls) {
                if (!summary.empty()) summary += ", ";
                summary += tc.name + "(" + tc.arguments + ")";
            }
            summary_conv.add_assistant("[called tools: " + summary + "]");
        } else if (msg.role == "tool" && msg.content) {
            // Tool results: just note the size — the content is waste now
            summary_conv.add_tool(msg.tool_call_id,
                "[tool result: " + std::to_string(msg.content->size()) + " bytes]");
        }
    }

    json payload = {
        {"model", model_},
        {"reasoning_effort", reasoning_effort_},
        {"messages", summary_conv.to_openai_messages()},
        {"stream", false},
        {"max_tokens", static_cast<int>(std::min(max_tokens, size_t(1024)))},
    };

    auto result = client_.chat(payload);
    if (!result) {
        return std::nullopt;
    }

    try {
        auto content = (*result)["choices"][0]["message"]["content"];
        return content.get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}


