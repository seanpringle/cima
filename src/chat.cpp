#include "chat.h"
#include "jobs.h"
#include "subagent.h"

#include <future>

ChatSession::ChatSession(Config config, TabType tab_type)
    : model_(std::move(config.model)), safe_dir_(std::move(config.safe_dir)),
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

    // Register job tools for both types, but omit close_job for builders
    if (tab_type_ == TabType::Planner) {
        add_job_tools(tools_);
    } else {
        // Register all job tools except close_job and edit_job
        tools_.add(make_open_job_tool());
        tools_.add(make_list_jobs_tool());
        tools_.add(make_read_job_tool());
        tools_.add(make_comment_job_tool());
    }

    // ── Subagent delegation tool (available to all tab types) ──
    {
        Tool t;
        t.name = "run_subagent";
        t.permission = ToolPermission::Internal;
        t.description =
            "Delegate a task to a subagent with a fresh context. "
            "Use this for research, exploration, or any task that "
            "benefits from isolated context or a different model.\n\n"
            "Available subagents:\n"
            "  explore — read-only research assistant (file reading, "
            "web search, git status/diff/log)\n"
            "  general — full-access assistant (all tools except "
            "subagent delegation)\n\n"
            "The subagent runs independently and returns its findings "
            "as a tool result. The subagent conversation is NOT merged "
            "into the primary context — only the final result is returned.";
        t.parameters = {{"type", "object"},
            {"properties",
                {{"name",
                    {{"type", "string"},
                        {"enum", {"explore", "general"}},
                        {"description", "Which subagent to use"}}},
                    {"task",
                        {{"type", "string"},
                            {"description",
                                "The task or question for the subagent"}}},
                    {"model",
                        {{"type", "string"},
                            {"description",
                                "Override model (optional, defaults to "
                                "primary session's model)"}}}}},
            {"required", {"name", "task"}}};
        // Capture the config by value for the api_base/api_key fallback.
        t.execute = [this, config](const json& args) -> Result<std::string> {
            return run_subagent_(args);
        };
        tools_.add(std::move(t));
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

// ===================================================================
// Subagent delegation
// ===================================================================

Result<std::string> ChatSession::run_subagent_(const json& args) {
    auto name = args.value("name", std::string());
    auto task = args.value("task", std::string());
    auto model_override = args.value("model", std::string());

    if (name.empty()) {
        return std::unexpected("subagent name is required");
    }
    if (task.empty()) {
        return std::unexpected("task is required");
    }

    // Look up the subagent definition
    const auto* def = find_builtin_subagent(name);
    if (!def) {
        return std::unexpected(
            "unknown subagent: " + name +
            ". Available: explore, general");
    }

    // Determine endpoint override (default to primary's if not specified)
    std::string api_base = def->api_base.empty() ? "" : def->api_base;
    std::string api_key = def->api_key.empty() ? "" : def->api_key;

    if (output_cb_) {
        output_cb_("[Subagent: " + def->display_name + " started]",
            OutputType::ToolInvocation);
    }

    auto result = run_subagent_session_(
        name, task, model_override, api_base, api_key);

    if (output_cb_) {
        output_cb_("[Subagent: " + def->display_name + " finished]",
            OutputType::ToolInvocation);
    }

    if (!result) {
        return std::unexpected(result.error());
    }
    return result->content;
}

Result<ChatResult> ChatSession::run_subagent_session_(
    const std::string& subagent_name,
    const std::string& task,
    const std::string& model_override,
    const std::string& api_base_override,
    const std::string& api_key_override) {

    // Determine which model to use
    std::string subagent_model = model_override.empty() ? model_ : model_override;

    // Determine which endpoint to use
    std::string subagent_api_base;
    std::string subagent_api_key;
    if (api_base_override.empty()) {
        // Use primary's endpoint: need the base URL without "/chat/completions"
        subagent_api_base = client_.url();
        auto suffix = subagent_api_base.rfind("/chat/completions");
        if (suffix != std::string::npos) {
            subagent_api_base = subagent_api_base.substr(0, suffix);
        }
        // Inherit the primary's API key
        subagent_api_key = api_key_;
    } else {
        subagent_api_base = api_base_override;
        subagent_api_key = api_key_override.empty() ? api_key_ : api_key_override;
    }

    // Create a ChatClient for the subagent (always new to avoid state sharing)
    ChatClient subagent_client(subagent_api_base, subagent_api_key);

    // Look up the subagent definition for system prompt and tool config
    auto* def = find_builtin_subagent(subagent_name);
    std::string system_prompt = def ? def->system_prompt
        : "You are a helpful assistant.";

    Conversation subagent_conv(system_prompt);
    subagent_conv.add_user(task);

    // Build the subagent's tool registry
    ToolRegistry subagent_tools;

    // Determine which tools the subagent gets
    std::set<std::string> allowed_tools;
    if (def) {
        if (def->name == "explore") {
            // Read-only tools only
            allowed_tools = tools_.tool_names_by_permission(ToolPermission::ReadOnly);
        } else {
            // All tools except run_subagent
            for (const auto& t : tools_.tools()) {
                if (t.name != "run_subagent") {
                    allowed_tools.insert(t.name);
                }
            }
        }
    } else {
        // Fallback: all tools except run_subagent
        for (const auto& t : tools_.tools()) {
            if (t.name != "run_subagent") {
                allowed_tools.insert(t.name);
            }
        }
    }

    // Register the allowed tools on the subagent registry
    for (const auto& t : tools_.tools()) {
        if (allowed_tools.count(t.name)) {
            subagent_tools.add(t);
        }
    }

    // Subagent loop
    const int max_subagent_iters = 25;
    std::string last_content;
    std::vector<std::string> trace;

    for (int iter = 0; iter < max_subagent_iters; iter++) {
        if (g_interrupted) {
            return std::unexpected("Interrupted during subagent execution");
        }

        json payload = {
            {"model", subagent_model},
            {"messages", subagent_conv.to_openai_messages()},
            {"tools", subagent_tools.to_openai_tools(&allowed_tools)},
            {"stream", false},
        };

        auto result = subagent_client.chat(payload);
        if (!result) {
            auto msg = result.error();
            auto raw = subagent_client.last_raw_response();
            if (!raw.empty()) {
                msg += " | raw: " + raw.substr(0, 500);
            }
            return std::unexpected(std::move(msg));
        }

        const json& data = *result;
        if (!data.contains("choices") || data["choices"].empty()) {
            return std::unexpected("subagent: empty response from API");
        }

        const auto& choice = data["choices"][0];
        const auto& message = choice["message"];

        // Extract content
        std::string content;
        auto c_it = message.find("content");
        if (c_it != message.end() && c_it->is_string()) {
            content = c_it->get<std::string>();
        }

        // Extract tool calls
        auto tc_it = message.find("tool_calls");
        if (tc_it != message.end() && tc_it->is_array() && !tc_it->empty()) {
            std::vector<ToolCall> calls;
            for (const auto& tc : *tc_it) {
                ToolCall call;
                call.index = tc.value("index", 0);
                call.id = tc.value("id", std::string());
                if (tc.contains("function")) {
                    call.name = tc["function"].value("name", std::string());
                    call.arguments = tc["function"].value("arguments", std::string());
                }
                calls.push_back(std::move(call));
            }

            // Add assistant message with tool calls to conversation
            subagent_conv.add_assistant("", "", calls);

            // Execute each tool
            for (const auto& call : calls) {
                if (g_interrupted) {
                    return std::unexpected("Interrupted during subagent tool execution");
                }

                std::string trace_line = "#" + std::to_string(iter + 1) +
                    " " + call.name + "(" + call.arguments + ")";
                trace.push_back(trace_line);

                auto tr = subagent_tools.execute(call.name, call.arguments);
                std::string result_str = tr ? *tr : tr.error();
                subagent_conv.add_tool(call.id, result_str);

                // Also add tool result to trace (truncated)
                if (result_str.size() > 200) {
                    trace.push_back("  -> (" + std::to_string(result_str.size()) + " bytes)");
                } else {
                    trace.push_back("  -> " + result_str);
                }
            }
            continue;
        }

        // Content response — we're done
        last_content = content;
        // Build the result with a trace
        std::string result_body;
        result_body += "--- Subagent result ---\n\n";
        result_body += last_content;
        if (!trace.empty()) {
            result_body += "\n\n--- Subagent trace ---\n";
            for (const auto& line : trace) {
                result_body += line + "\n";
            }
        }
        return ChatResult{std::move(result_body), std::string()};
    }

    return std::unexpected(
        "Subagent reached maximum iterations (" +
        std::to_string(max_subagent_iters) + ")");
}
