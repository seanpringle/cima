#include "chat.h"
#include "plan.h"
#include "tools/lua_engine.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

// Names of all CMake tools — conditionally published when CMakeLists.txt
// exists in the workspace.
static const std::unordered_set<std::string> cmake_tool_names = {
    "cmake_configure",
    "cmake_build",
    "cmake_ctest",
};

ChatSession::ChatSession(
    const Config& config, const Provider& provider, CancellationToken cancelled,
    std::shared_ptr<GatingState> gates)
    : config_(config), model_(provider.model), reasoning_effort_(provider.reasoning_effort),
      provider_name_(provider.name),
      safe_dir_(std::make_shared<std::string>(std::filesystem::current_path().string())),
      api_base_(provider.api_base), api_key_(provider.api_key),
      max_iterations_(config.max_tool_iterations), context_limit_(provider.context_limit),
      system_prompt_(config.SYSTEM_PROMPT), client_(provider.api_base, provider.api_key),
      file_modified_cb_(std::make_shared<FileModifiedCallback>()),
      cancelled_(cancelled ? std::move(cancelled) : make_cancellation_token()),
      gates_(gates ? std::move(gates) : std::make_shared<GatingState>()),
      lua_state_(luaL_newstate(), lua_close),
      lua_mutex_(std::make_shared<std::mutex>()) {
    // Share the cancellation token with tools and client
    tools_.set_cancelled(cancelled_);
    client_.set_cancelled(cancelled_);

    tools_.add_defaults(safe_dir_, config, /*include_write=*/true, *file_modified_cb_, tool_logs_);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(::plan));
    tools_.add(make_read_plan_tool(::plan));

    // Tool log buffer for spilling large outputs
    tool_logs_ = std::make_shared<std::vector<std::string>>();

    // CMake tools — always registered; conditionally published in run_once().
    tools_.add(make_cmake_configure_tool(safe_dir_, config.cmake_configure_timeout,
        cancelled_, tool_logs_));
    tools_.add(make_cmake_build_tool(safe_dir_, config.cmake_build_timeout,
        cancelled_, tool_logs_));
    tools_.add(make_cmake_ctest_tool(safe_dir_, config.cmake_ctest_timeout,
        cancelled_, tool_logs_));

    // Custom cmd_tools — registered from config.
    for (const auto& ct : config.cmd_tools) {
        tools_.add(make_cmd_tool(ct.name, ct.description, ct.command,
                                 safe_dir_, config.bash_timeout, cancelled_, tool_logs_));
    }

    // view_tool_output — always available to all agents
    tools_.add(make_view_tool_output_tool(tool_logs_));

    // ── Lua tool — always available (removed for subagents in create_subagent) ──
    if (lua_state_) {
        luaL_openlibs(lua_state_.get());
        tools_.add(make_lua_tool(lua_state_, lua_mutex_, config_.lua_timeout));
    }
}

// ===================================================================
// Subagent factory
// ===================================================================

std::unique_ptr<ChatSession> ChatSession::create_subagent(
    const Config& config, const Provider& provider, bool read_only,
    CancellationToken cancelled, std::shared_ptr<GatingState> gates) {
    // Build a simpler system prompt for subagents
    std::string sp = Config::SUBAGENT_SYSTEM_PROMPT;

    if (!read_only) {
        sp += "You have access to read and write file tools, and git tools.\n";
    }
    // NOTE: CMAKE_PROMPT_SNIPPET is NOT baked here — it's added dynamically
    // in build_effective_prompt() based on gates_->cmake_enabled.

    // Create the session, sharing gates with the primary if provided.
    auto session = std::make_unique<ChatSession>(
        config, provider, std::move(cancelled), std::move(gates));
    session->system_prompt_ = std::move(sp);
    session->is_read_only_ = read_only;

    // Remove bash, write_plan, and lua tools (read_plan is kept for subagents)
    session->tools_.remove("run_bash");
    session->tools_.remove("write_plan");
    session->tools_.remove("lua");

    if (read_only) {
        // File write tools
        session->tools_.remove("write_file");
        session->tools_.remove("edit_file");
        session->tools_.remove("delete_file");
        session->tools_.remove("move_file");
        session->tools_.remove("rename_file");
        // Git write tools
        session->tools_.remove("git_add");
        session->tools_.remove("git_commit");
        session->tools_.remove("git_restore");
        session->tools_.remove("git_show");
        // CMake tools — read-only subagents never get these
        session->tools_.remove("cmake_configure");
        session->tools_.remove("cmake_build");
        session->tools_.remove("cmake_ctest");
    }

    return session;
}

void ChatSession::set_provider(const Provider& provider) {
    provider_name_ = provider.name;
    api_base_ = provider.api_base;
    api_key_ = provider.api_key;
    model_ = provider.model;
    reasoning_effort_ = provider.reasoning_effort;
    context_limit_ = provider.context_limit;
    context_limit_discovered_ = false; // will re-discover on next run
    client_.set_api_base(provider.api_base);
    client_.set_api_key(provider.api_key);
}

bool ChatSession::has_cmake_project() const {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(*safe_dir_) / "CMakeLists.txt", ec);
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

    std::string cache_key = client_.url() + ":" + model_;
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
    // Dynamic snippet injection based on active gates.
    // Read-write subagents share the primary's gates, so they get the
    // snippet when the primary has cmake enabled. Read-only subagents
    // never get it (their gates_ is a fresh default with cmake_enabled=false,
    // and cmake tools were removed from their registry).
    if (gates_->cmake_enabled && has_cmake_project()) {
        prompt += Config::CMAKE_PROMPT_SNIPPET;
    }
    if (mcp_registry_.has_running_servers()) {
        prompt += "\n## MCP tools\n\n"
                  "Tools from external MCP servers are available.\n"
                  "These are listed among your tools with a \"mcp_<servername>_\" prefix.\n"
                  "Use them as you would any other tool.\n";
    }
    return prompt;
}

std::set<std::string> ChatSession::filter_allowed_tools() const {
    std::set<std::string> allowed;
    for (const auto& t : tools_.tools()) {
        bool include = true;
        if (cmake_tool_names.count(t.name) && (!gates_->cmake_enabled || !has_cmake_project()))
            include = false;
        if (t.name == "run_bash" && !gates_->bash_enabled)
            include = false;
        // Custom cmd_* tools: each gated individually by gates_->custom_tools.
        if (t.name.rfind("cmd_", 0) == 0) {
            auto it = gates_->custom_tools.find(t.name);
            if (it == gates_->custom_tools.end() || !it->second)
                include = false;
        }
        if (include)
            allowed.insert(t.name);
    }
    return allowed;
}

json ChatSession::build_payload(const std::set<std::string>& allowed_tools) const {
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

    auto on_data = [&](const json& data) {
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

    auto stream_result = client_.stream_chat(payload, callbacks);
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
            try {
                tr = tools_.execute(call.name, call.arguments);
            } catch (const std::exception& e) {
                tr = std::unexpected(std::string(e.what()));
            }
            conversation_.add_tool(msg_id, call.id, tr ? *tr : tr.error());
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
            try {
                tr = tools_.execute(call.name, call.arguments);
            } catch (const std::exception& e) {
                tr = std::unexpected(std::string(e.what()));
            }
            conversation_.add_tool(msg_id, call.id, tr ? *tr : tr.error());
        }
    } else {
        // Parallel execution for read-only batch.
        // Launch ALL tools first, then collect ALL results.
        // This ensures no futures are abandoned if cancellation
        // happens mid-collection.
        std::vector<std::future<Result<std::string>>> futures;
        futures.reserve(calls.size());
        for (size_t i = 0; i < calls.size(); i++) {
            futures.push_back(std::async(std::launch::async,
                [&, i] { return tools_.execute(calls[i].name, calls[i].arguments); }));
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

Result<void> ChatSession::compact() {
    // Snapshot the current messages (skip system prompt — that's added at build time)
    auto& msgs = conversation_.messages();

    // Don't compact if there's nothing meaningful
    if (msgs.size() < 2) {
        return {}; // Nothing to compact
    }

    // Build a summarization prompt from the conversation
    std::string summary_prompt =
        "Please provide a comprehensive summary of the following conversation. "
        "Preserve all important context, decisions, code changes, file paths, "
        "and outstanding tasks. Be detailed enough that the conversation can "
        "continue seamlessly from this summary.\n\n---\n";

    for (const auto& msg : msgs) {
        if (msg.role == "system")
            continue; // skip system messages

        summary_prompt += "[" + msg.role + "]: ";
        if (msg.content.has_value()) {
            summary_prompt += *msg.content;
        }
        if (!msg.reasoning_content.empty()) {
            summary_prompt += "\n[reasoning]: " + msg.reasoning_content;
        }
        for (const auto& tc : msg.tool_calls) {
            summary_prompt += "\n[tool_call: " + tc.name + "(" + tc.arguments + ")]";
            if (!tc.result.empty()) {
                summary_prompt += "\n[tool_result: " + tc.result + "]";
            }
        }
        summary_prompt += "\n";
    }

    // Send to LLM via client_
    // Build the full tool list so the model sees a consistent prompt.
    // The model won't actually use tools in a summarization request,
    // but some models may behave oddly when tools are suddenly absent.
    json payload = {{"model", model_},
        {"messages",
            json::array(
                {{{"role", "system"},
                     {"content",
                         sanitize_utf8(system_prompt_ +
                             (has_cmake_project() ? std::string(Config::CMAKE_PROMPT_SNIPPET)
                                                  : ""))}},
                    {{"role", "user"}, {"content", sanitize_utf8(summary_prompt)}}})},
        {"tools", tools_.to_openai_tools()},
        {"stream", true}};
    if (!reasoning_effort_.empty())
        payload["reasoning_effort"] = reasoning_effort_;
    payload["stream_options"] = {{"include_usage", true}};

    std::string summary;
    bool stream_errored = false;
    std::string stream_error;

    auto on_data = [&](const json& data) {
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

    auto stream_result = client_.stream_chat(payload, callbacks);
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

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void ChatSession::clear() {
    conversation_.clear();
    last_usage_ = Usage{}; // reset so context pct falls back to estimate(0)
}

// ===================================================================
// Session custom command registration
// ===================================================================

void ChatSession::register_custom_command(const std::string& name,
    const std::string& description, const std::string& command,
    int timeout_sec) {
    std::string tool_name = "cmd_" + name;
    // Remove config version if exists (session masks config)
    tools_.remove(tool_name);
    // Register session version
    tools_.add(make_cmd_tool(name, description, command,
                             safe_dir_, timeout_sec, cancelled_, tool_logs_));
    // Default to enabled
    set_custom_tool_enabled(tool_name, true);
}

void ChatSession::unregister_custom_command(const std::string& name) {
    std::string tool_name = "cmd_" + name;
    tools_.remove(tool_name);
    // Re-register config version if it exists
    for (const auto& ct : config_.cmd_tools) {
        if (ct.name == name) {
            tools_.add(make_cmd_tool(ct.name, ct.description, ct.command,
                                     safe_dir_, config_.bash_timeout, cancelled_, tool_logs_));
            break;
        }
    }
}
