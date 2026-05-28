#include "chat.h"
#include "plan.h"
#include "skill.h"

#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

ChatSession::ChatSession(ConfigPtr config,
    const Provider& provider,
    CancellationToken cancelled,
    std::shared_ptr<GatingState> gates,
    PlanBoard* plan)
    : config_(std::move(config)), plan_(plan ? PlanBoardPtr(plan, [](PlanBoard*){}) : std::make_shared<PlanBoard>()), model_(provider.model), reasoning_effort_(provider.reasoning_effort),
      provider_name_(provider.name),
      safe_dir_(std::make_shared<std::string>(std::filesystem::current_path().string())),
      api_base_(provider.api_base), api_key_(provider.api_key),
      api_type_(provider.api_type),
      max_tokens_(provider.max_tokens),
      max_iterations_(kDefaultMaxToolIterations), context_limit_(provider.context_limit),
      system_prompt_(config_->SYSTEM_PROMPT), client_(provider.api_base, provider.api_key),
      file_modified_cb_(std::make_shared<FileModifiedCallback>()),
      cancelled_(cancelled ? std::move(cancelled) : make_cancellation_token()),
      gates_(gates ? std::move(gates) : std::make_shared<GatingState>()) {
    // Set the api_type on the client
    client_.set_api_type(provider.api_type);
    // Share the cancellation token with tools and client
    tools_.set_cancelled(cancelled_);
    client_.set_cancelled(cancelled_);

    tools_.add_defaults(safe_dir_, *config_, /*include_write=*/true, *file_modified_cb_);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(*plan_));
    tools_.add(make_read_plan_tool(*plan_));
}

// ===================================================================
// Subagent factory
// ===================================================================

std::unique_ptr<ChatSession> ChatSession::create_subagent(ConfigPtr config,
    const Provider& provider,
    bool read_only,
    CancellationToken cancelled,
    std::shared_ptr<GatingState> gates,
    PlanBoard* plan) {
    // Build a simpler system prompt for subagents
    std::string sp = Config::SUBAGENT_SYSTEM_PROMPT;

    if (!read_only) {
        sp += "You have access to read and write file tools, and git tools.\n";
    }

    // Create the session with the given gates (nullptr = fresh default).
    auto session =
        std::make_unique<ChatSession>(config, provider, std::move(cancelled), std::move(gates), plan);
    session->system_prompt_ = std::move(sp);
    session->is_read_only_ = read_only;

    // Remove write_plan tool (read_plan is kept for subagents).
    session->tools_.remove("write_plan");

    if (read_only) {
        // File write tools
        session->tools_.remove("write_file");
        session->tools_.remove("edit_file");
    }

    return session;
}

void ChatSession::set_provider(const Provider& provider) {
    provider_name_ = provider.name;
    api_base_ = provider.api_base;
    api_key_ = provider.api_key;
    api_type_ = provider.api_type;
    max_tokens_ = provider.max_tokens;
    model_ = provider.model;
    reasoning_effort_ = provider.reasoning_effort;
    context_limit_ = provider.context_limit;
    context_limit_discovered_ = false; // will re-discover on next run
    client_.set_api_base(provider.api_base);
    client_.set_api_key(provider.api_key);
    client_.set_api_type(provider.api_type);
}

int ChatSession::context_usage_percent() const {
    if (context_limit_ <= 0)
        return 0;
    // Use the API-reported token count when available (more accurate),
    // fall back to the conversation estimate after restart.
    int tokens = last_usage_.total_tokens;
    if (tokens == 0) {
        tokens = static_cast<int>(conversation_.estimate_total_tokens());
    }
    return static_cast<int>(tokens * 100 / context_limit_);
}

// ===================================================================
// run_once() helpers
// ===================================================================

void ChatSession::discover_context_limit() {
    if (context_limit_discovered_)
        return;

    static std::mutex cache_mutex;
    static std::unordered_map<std::string, int> cache;

    std::string cache_key = client_.url() + ":" + model_ + ":" + effective_api_type();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(cache_key);
        if (it != cache.end()) {
            context_limit_ = it->second;
            context_limit_discovered_ = true;
            return;
        }
    }

    int discovered = client_.fetch_model_context_limit(model_);
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (discovered > 0) {
        cache[cache_key] = discovered;
        context_limit_ = discovered;
    }
    context_limit_discovered_ = true;
}

std::string ChatSession::build_effective_prompt() const {
    std::string prompt = system_prompt_;
    if (mcp_registry_.has_running_servers()) {
        prompt += "\n## MCP tools\n\n"
                  "Tools from external MCP servers are available.\n"
                  "These are listed among your tools with a \"mcp_<servername>_\" prefix.\n"
                  "Use them as you would any other tool.\n";

        // Append a table showing running servers and their descriptions.
        auto servers = mcp_registry_.running_servers();
        if (!servers.empty()) {
            prompt += "\n## MCP Servers\n\n"
                      "| Server | Description |\n"
                      "| --- | --- |\n";
            for (const auto& s : servers) {
                prompt += "| " + s.name + " | " + s.description + " |\n";
            }
        }
    }

    // ── Skills section (Level 1 progressive disclosure) ──
    if (skill_registry_ && skill_registry_->size() > 0) {
        prompt += "\n## Available Skills\n\n"
                  "Skills are reusable instruction sets that teach the agent "
                  "specialised knowledge and workflows. Below is a list of "
                  "available skills. Load one with the `load_skill` tool when "
                  "its task matches your goal.\n\n"
                  "| Skill | Description |\n"
                  "| --- | --- |\n";
        for (const auto& s : skill_registry_->skills()) {
            // Escape pipe characters in name/description to avoid breaking the table
            auto esc = [](std::string v) {
                size_t p = 0;
                while ((p = v.find('|', p)) != std::string::npos) {
                    v.replace(p, 1, "\\|");
                    p += 2;
                }
                return v;
            };
            prompt += "| " + esc(s.name) + " | " + esc(s.description) + " |\n";
        }
    }

    // Append any dynamically-loaded skill content.
    prompt += conversation_.get_appended_system();

    return prompt;
}

bool ChatSession::is_tool_allowed(const std::string& name) const {
    // Generic per-tool gate from tool_gates map.
    {
        auto it = gates_->tool_gates.find(name);
        if (it != gates_->tool_gates.end() && !it->second)
            return false;
    }
    return true;
}

std::set<std::string> ChatSession::filter_allowed_tools() const {
    std::set<std::string> allowed;
    for (const auto& t : tools_.tools()) {
        if (is_tool_allowed(t.name))
            allowed.insert(t.name);
    }
    return allowed;
}

json ChatSession::build_payload(const std::set<std::string>& allowed_tools) const {
    if (effective_api_type() == "anthropic") {
        auto ap = conversation_.build_anthropic_payload(build_effective_prompt());
        json payload;
        payload["model"] = model_;
        payload["system"] = ap["system"];
        payload["messages"] = ap["messages"];
        payload["max_tokens"] = max_tokens_ > 0 ? max_tokens_ : (context_limit_ > 0 ? std::min(context_limit_ / 4, 8192) : 4096);
        payload["stream"] = true;
        auto tools = tools_.to_anthropic_tools(&allowed_tools);
        if (!tools.empty())
            payload["tools"] = tools;
        // Map reasoning_effort to Anthropic thinking budget.
        if (!reasoning_effort_.empty() && reasoning_effort_ != "low") {
            int budget = 2000;
            if (reasoning_effort_ == "high") budget = 4000;
            payload["thinking"] = {{"type", "enabled"}, {"budget_tokens", budget}};
        }
        return payload;
    }

    // OpenAI-compatible payload
    json payload = {{"model", model_},
        {"messages", conversation_.build_openai_payload(build_effective_prompt())},
        {"tools", tools_.to_openai_tools(&allowed_tools)},
        {"stream", true}};
    if (!reasoning_effort_.empty())
        payload["reasoning_effort"] = reasoning_effort_;
    payload["stream_options"] = {{"include_usage", true}};
    return payload;
}

Result<ChatSession::StreamResult> ChatSession::stream_chat(const json& payload) {
    StreamResult result;
    ToolAccumulator tool_acc;
    bool stream_errored = false;
    std::string stream_error;

    auto on_data = [&](const std::string& /*event*/, const json& data) {
        // Capture token usage if present (may appear in the final chunk).
        auto usage_it = data.find("usage");
        if (usage_it != data.end() && usage_it->is_object()) {
            try {
                last_usage_ = usage_it->get<Usage>();
            } catch (...) {
            }
        }

        if (!data.contains("choices") || data["choices"].empty())
            return;
        const auto& delta = data["choices"][0]["delta"];

        auto rc_it = delta.find("reasoning_content");
        if (rc_it != delta.end() && rc_it->is_string()) {
            auto text = rc_it->get<std::string>();
            result.reasoning += text;
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
            result.content += text;
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

    Result<void> stream_result;
    if (effective_api_type() == "anthropic") {
        stream_result = client_.stream_chat_anthropic(payload, callbacks);
    } else {
        stream_result = client_.stream_chat(payload, callbacks);
    }
    if (!stream_result) {
        return std::unexpected(stream_result.error());
    }

    if (stream_errored && result.content.empty()) {
        return std::unexpected(stream_error);
    }

    result.calls = tool_acc.finalize();
    return result;
}

Result<void> ChatSession::execute_tool_calls(
    int64_t msg_id, const std::vector<ToolCall>& calls, int remaining_iters) {
    (void)remaining_iters;

    if (*cancelled_) {
        return std::unexpected("Interrupted during tool execution");
    }

    auto write_tools = tools_.tool_names_by_permission(ToolPermission::Write);
    bool has_write = false;
    for (const auto& call : calls) {
        if (write_tools.count(call.name)) {
            has_write = true;
            break;
        }
    }

    // Helper: produce an error result for a disabled tool
    auto disabled = [&](const ToolCall& call) -> Result<std::string> {
        return std::unexpected("tool '" + call.name + "' is disabled");
    };

    if (calls.size() <= 1) {
        // Single call — simple execution
        for (const auto& call : calls) {
            if (*cancelled_) {
                return std::unexpected("Interrupted during tool execution");
            }
            if (output_cb_) {
                output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                    OutputType::ToolInvocation);
            }
            Result<std::string> tr;
            if (!is_tool_allowed(call.name)) {
                tr = disabled(call);
            } else {
                try {
                    tr = tools_.execute(call.name, call.arguments);
                } catch (const std::exception& e) {
                    tr = std::unexpected(std::string(e.what()));
                }
            }
            conversation_.add_tool(msg_id, call.id, tr ? *tr : tr.error());
            if (output_cb_) {
                output_cb_(tr ? *tr : tr.error(), OutputType::ToolResult);
            }
        }
    } else if (has_write) {
        // Serial execution for batch with write tools
        // (avoids race conditions on shared resources e.g. .git/index.lock)
        for (const auto& call : calls) {
            if (*cancelled_) {
                return std::unexpected("Interrupted during tool execution");
            }
            if (output_cb_) {
                output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                    OutputType::ToolInvocation);
            }
            Result<std::string> tr;
            if (!is_tool_allowed(call.name)) {
                tr = disabled(call);
            } else {
                try {
                    tr = tools_.execute(call.name, call.arguments);
                } catch (const std::exception& e) {
                    tr = std::unexpected(std::string(e.what()));
                }
            }
            conversation_.add_tool(msg_id, call.id, tr ? *tr : tr.error());
            if (output_cb_) {
                output_cb_(tr ? *tr : tr.error(), OutputType::ToolResult);
            }
        }
    } else {
        // Parallel execution for read-only batch.
        // Launch ALL tools first, then collect ALL results.
        // This ensures no futures are abandoned if cancellation
        // happens mid-collection.
        std::vector<std::future<Result<std::string>>> futures;
        futures.reserve(calls.size());
        for (size_t i = 0; i < calls.size(); i++) {
            // Capture by value to avoid dangling references in the async thread.
            std::string tool_name = calls[i].name;
            std::string tool_args = calls[i].arguments;
            futures.push_back(std::async(std::launch::async,
                [this, tool_name, tool_args] {
                    if (!this->is_tool_allowed(tool_name))
                        return Result<std::string>(std::unexpected("tool '" + tool_name + "' is disabled"));
                    return this->tools_.execute(tool_name, tool_args);
                }));
        }

        std::vector<Result<std::string>> results;
        results.reserve(calls.size());
        for (auto& f : futures) {
            try {
                results.push_back(f.get());
            } catch (const std::exception& e) {
                results.push_back(std::unexpected(std::string(e.what())));
            }
        }

        if (*cancelled_) {
            return std::unexpected("Interrupted during tool execution");
        }

        for (size_t i = 0; i < calls.size(); i++) {
            if (output_cb_) {
                output_cb_("\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments + ")",
                    OutputType::ToolInvocation);
            }
            conversation_.add_tool(
                msg_id, calls[i].id, results[i] ? *results[i] : results[i].error());
            if (output_cb_) {
                output_cb_(results[i] ? *results[i] : results[i].error(), OutputType::ToolResult);
            }
        }
    }

    return {};
}

// ===================================================================
// run_once() — high-level orchestrator
// ===================================================================

Result<ChatResult> ChatSession::run_once(const std::string& user_input) {
    discover_context_limit();

    auto turn_snapshot = conversation_.message_count();
    conversation_.add_user(user_input);

    auto rollback = [&] { conversation_.truncate_conversation(turn_snapshot); };

    std::string last_content;
    std::string last_reasoning;
    bool produced_content = false;

    try {
        for (int iter = 0; iter < max_iterations_; iter++) {
            auto allowed = filter_allowed_tools();
            auto payload = build_payload(allowed);

            auto stream_result = stream_chat(payload);
            if (!stream_result) {
                rollback();
                auto msg = stream_result.error();
                auto raw = client_.last_raw_response();
                if (!raw.empty()) {
                    msg += " | raw: " + raw.substr(0, 500);
                }
                return std::unexpected(std::move(msg));
            }

            auto& [content, reasoning, calls] = *stream_result;

            if (!calls.empty()) {
                auto msg_id = conversation_.add_assistant("", reasoning, calls);

                auto exec_result = execute_tool_calls(msg_id, calls, max_iterations_ - iter - 1);
                if (!exec_result) {
                    rollback();
                    return std::unexpected(exec_result.error());
                }
                continue;
            }

            // No tool calls — content response
            conversation_.add_assistant(content, reasoning);
            last_content = content;
            last_reasoning = reasoning;
            produced_content = true;
            break;
        }
    } catch (const std::exception& e) {
        rollback();
        return std::unexpected(std::string(e.what()));
    }

    if (!produced_content) {
        std::string msg =
            "Tool call budget exhausted (" + std::to_string(max_iterations_) + " iterations).";
        conversation_.add_assistant(msg, "");
        last_content = msg;
        last_reasoning = "";
        produced_content = true;
    }

    // ── Auto-compaction at 90% context usage ──
    if (context_usage_percent() >= 90) {
        if (output_cb_)
            output_cb_("(compacting...)", OutputType::ToolInvocation);
        auto compact_result = compact();
        if (!compact_result) {
            if (output_cb_)
                output_cb_(
                    "compact() failed: " + compact_result.error(), OutputType::ToolInvocation);
        }
    }

    return ChatResult{std::move(last_content), std::move(last_reasoning)};
}

// ===================================================================
// Conversation compaction
// ===================================================================

Result<void> ChatSession::compact() {
    // Snapshot the current messages (skip system prompt — that's added at build time)
    auto& msgs = conversation_.messages();

    // Don't compact if there's nothing meaningful
    if (msgs.size() < 2) {
        return {}; // Nothing to compact
    }

    // ── Truncate older tool results to prevent unbounded prompt growth ──
    // Keep full results for recent exchanges; mark older ones as truncated.
    size_t max_tool_results = 10;

    // First pass: count total tool calls so we know which are "recent"
    size_t total_tool_calls = 0;
    for (auto& msg : msgs) {
        total_tool_calls += msg.tool_calls.size();
    }

    // Second pass: truncate tool results older than the last max_tool_results
    size_t result_count = 0;
    size_t truncate_before = (total_tool_calls > max_tool_results)
        ? (total_tool_calls - max_tool_results) : 0;
    for (auto& msg : msgs) {
        for (auto& tc : msg.tool_calls) {
            if (result_count < truncate_before && tc.result.size() > 500) {
                tc.result = "(truncated — see earlier in conversation)";
            }
            result_count++;
        }
    }

    // ── Build a proper multi-message payload for the summarization request ──
    const std::string compact_sys_prompt =
        "You are summarizing a conversation for context retention. "
        "Provide a comprehensive, detailed summary that preserves ALL significant "
        "context from the entire conversation — not just the most recent exchanges. "
        "Include important decisions, code changes, file paths, tool outputs, and "
        "outstanding tasks. The summary must be detailed enough that another session "
        "can continue seamlessly from this point.";

    const std::string compact_user_msg =
        "Please provide a comprehensive summary of the conversation above. "
        "Preserve all important context, decisions, code changes, file paths, "
        "and outstanding tasks. Be detailed enough that the conversation can "
        "continue seamlessly from this summary.";

    json payload;
    if (effective_api_type() == "anthropic") {
        auto ap = conversation_.build_anthropic_payload(compact_sys_prompt);
        payload["model"] = model_;
        payload["system"] = ap["system"];
        // Insert the compact system prompt as the system field
        payload["system"] = compact_sys_prompt;
        payload["messages"] = ap["messages"];
        // Append the summarization user message
        payload["messages"].push_back({{"role", "user"}, {"content", compact_user_msg}});
        payload["max_tokens"] = max_tokens_ > 0 ? max_tokens_ : (context_limit_ > 0 ? std::min(context_limit_ / 4, 8192) : 4096);
        payload["stream"] = true;
        auto tools = tools_.to_anthropic_tools();
        if (!tools.empty()) payload["tools"] = tools;
    } else {
        json messages = json::array();
        messages.push_back({{"role", "system"}, {"content", compact_sys_prompt}});

        for (const auto& msg : msgs) {
            if (msg.role == "system") continue;
            if (msg.tool_calls.empty()) {
                json m{{"role", msg.role}};
                m["content"] = msg.content.value_or("");
                if (msg.role == "assistant" && !msg.reasoning_content.empty()) {
                    m["reasoning_content"] = msg.reasoning_content;
                }
                messages.push_back(std::move(m));
            } else {
                json assistant_msg{{"role", "assistant"}};
                if (msg.content.has_value() && !msg.content->empty()) {
                    assistant_msg["content"] = *msg.content;
                }
                if (!msg.reasoning_content.empty()) {
                    assistant_msg["reasoning_content"] = msg.reasoning_content;
                }
                json tc_arr = json::array();
                for (const auto& tc : msg.tool_calls) {
                    json tc_json;
                    tc_json["id"] = tc.id;
                    tc_json["type"] = "function";
                    tc_json["function"] = {{"name", tc.name}, {"arguments", tc.arguments}};
                    tc_arr.push_back(std::move(tc_json));
                }
                assistant_msg["tool_calls"] = std::move(tc_arr);
                messages.push_back(std::move(assistant_msg));
                for (const auto& tc : msg.tool_calls) {
                    json tr;
                    tr["role"] = "tool";
                    tr["tool_call_id"] = tc.id;
                    tr["content"] = tc.result.empty() ? "(empty result)" : tc.result;
                    messages.push_back(std::move(tr));
                }
            }
        }
        messages.push_back({{"role", "user"}, {"content", compact_user_msg}});

        payload["model"] = model_;
        payload["messages"] = std::move(messages);
        payload["tools"] = tools_.to_openai_tools();
        payload["stream"] = true;
        if (!reasoning_effort_.empty())
            payload["reasoning_effort"] = reasoning_effort_;
        payload["stream_options"] = {{"include_usage", true}};
    }

    std::string summary;
    bool stream_errored = false;
    std::string stream_error;

    auto on_data = [&](const std::string& /*event*/, const json& data) {
        if (!data.contains("choices") || data["choices"].empty())
            return;
        const auto& delta = data["choices"][0]["delta"];

        auto c_it = delta.find("content");
        if (c_it != delta.end() && c_it->is_string()) {
            summary += c_it->get<std::string>();
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

    Result<void> stream_result;
    if (effective_api_type() == "anthropic") {
        stream_result = client_.stream_chat_anthropic(payload, callbacks);
    } else {
        stream_result = client_.stream_chat(payload, callbacks);
    }
    if (!stream_result) {
        return std::unexpected(stream_result.error());
    }
    if (stream_errored && summary.empty()) {
        return std::unexpected(stream_error);
    }

    // Replace the conversation with just the summary
    conversation_.replace_with_summary(summary);
    last_usage_ = Usage{}; // reset so UI falls back to conversation estimate

    if (output_cb_)
        output_cb_("(conversation compacted — summary follows)\n\n" + summary, OutputType::Content);

    return {};
}

// ===================================================================
// MCP server management
// ===================================================================

Result<void> ChatSession::start_mcp_server(const McpEndpoint& config) {
    auto result = mcp_registry_.start_server(config);
    if (!result) {
        return std::unexpected(
            std::string("Failed to start MCP server '") + config.name + "': " + result.error());
    }

    // Register all discovered tools in the ToolRegistry,
    // wiring execute to route through the McpRegistry.
    auto tools = mcp_registry_.all_tools();
    for (auto& tool : tools) {
        std::string namespaced_name = tool.name;
        tool.execute = [this, namespaced_name](const json& args) -> Result<std::string> {
            return mcp_registry_.execute_tool(namespaced_name, args);
        };
        tools_.add(std::move(tool));
    }

    return {};
}

void ChatSession::stop_mcp_server(const std::string& name) {
    // Remove all tools belonging to this server from ToolRegistry.
    std::string prefix = "mcp_" + name + "_";
    std::vector<std::string> to_remove;
    for (const auto& t : tools_.tools()) {
        if (t.name.rfind(prefix, 0) == 0) {
            to_remove.push_back(t.name);
        }
    }
    for (const auto& tname : to_remove) {
        tools_.remove(tname);
    }

    mcp_registry_.stop_server(name);
}

Result<void> ChatSession::start_custom_mcp_server(const McpEndpoint& config) {
    return start_mcp_server(config);
}

void ChatSession::stop_custom_mcp_server(const std::string& name) { stop_mcp_server(name); }

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void ChatSession::clear() {
    conversation_.clear();
    last_usage_ = Usage{}; // reset so context pct falls back to estimate(0)
}


