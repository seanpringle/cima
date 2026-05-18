#include "chat.h"
#include "plan.h"

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

ChatSession::ChatSession(const Config& config, const Provider& provider,
    CancellationToken cancelled)
    : model_(provider.model), reasoning_effort_(provider.reasoning_effort),
      provider_name_(provider.name),
      safe_dir_(std::make_shared<std::string>(
          std::filesystem::current_path().string())),
      api_base_(provider.api_base), api_key_(provider.api_key),
      max_iterations_(config.max_tool_iterations),
      context_limit_(provider.context_limit),
      system_prompt_(config.SYSTEM_PROMPT),
      client_(provider.api_base, provider.api_key),
      file_modified_cb_(std::make_shared<FileModifiedCallback>()),
      cancelled_(cancelled ? std::move(cancelled) : make_cancellation_token()) {
    // Share the cancellation token with tools and client
    tools_.set_cancelled(cancelled_);
    client_.set_cancelled(cancelled_);

    tools_.add_defaults(safe_dir_, config, /*include_write=*/true, *file_modified_cb_);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(::plan));
    tools_.add(make_read_plan_tool(::plan));

    // CMake tools — always registered; conditionally published in run_once().
    tools_.add(make_cmake_configure_tool(safe_dir_, config.cmake_configure_timeout, cancelled_));
    tools_.add(make_cmake_build_tool(safe_dir_, config.cmake_build_timeout, cancelled_));
    tools_.add(make_cmake_ctest_tool(safe_dir_, config.cmake_ctest_timeout, cancelled_));

}

// ===================================================================
// Subagent factory
// ===================================================================

std::unique_ptr<ChatSession> ChatSession::create_subagent(
    const Config& config, const Provider& provider, bool read_only,
    CancellationToken cancelled) {
    // Build a simpler system prompt for subagents
    std::string sp = Config::SUBAGENT_SYSTEM_PROMPT;

    if (!read_only) {
        sp += "You have access to read and write file tools, and git tools.\n";
    }

    // Check for CMake project and append CMake snippet if present
    std::error_code ec;
    if (std::filesystem::exists(
            std::filesystem::current_path() / "CMakeLists.txt", ec)) {
        sp += Config::CMAKE_PROMPT_SNIPPET;
    }

    // Create the session using the private constructor via a wrapper
    // We construct in-place since the constructor is complex.
    auto session = std::make_unique<ChatSession>(
        config, provider, std::move(cancelled));
    session->system_prompt_ = std::move(sp);

    // Remove bash and write_plan tools (read_plan is kept for subagents)
    session->tools_.remove("run_bash");
    session->tools_.remove("write_plan");

    // Conditionally remove write tools
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
    return std::filesystem::exists(
        std::filesystem::path(*safe_dir_) / "CMakeLists.txt", ec);
}

int ChatSession::context_usage_percent() const {
    if (context_limit_ <= 0) return 0;
    // Use the API-reported token count when available (more accurate),
    // fall back to the conversation estimate after restart.
    int tokens = last_usage_.total_tokens;
    if (tokens == 0) {
        tokens = static_cast<int>(conversation_.estimate_total_tokens());
    }
    return static_cast<int>(tokens * 100 / context_limit_);
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

    // ── Single turn processing ──
    // Snapshot before adding this turn's prompt.  If anything goes wrong
    // we roll back to here.
    auto turn_snapshot = conversation_.message_count();

    // Add the user input as a message
    conversation_.add_user(user_input);

    std::string last_content;
    std::string last_reasoning;
    bool produced_content = false;

    try {
        for (int iter = 0; iter < max_iterations_; iter++) {
            // Build effective system prompt (conditionally include CMake/MCP sections)
            std::string effective_prompt = system_prompt_;
            if (has_cmake_project()) {
                effective_prompt += Config::CMAKE_PROMPT_SNIPPET;
            }
            if (mcp_registry_.has_running_servers()) {
                effective_prompt +=
                    "\n## MCP tools\n\n"
                    "Tools from external MCP servers are available.\n"
                    "These are listed among your tools with a \"mcp_<servername>_\" prefix.\n"
                    "Use them as you would any other tool.\n";
            }

            // Filter tool list: only include CMake/bash tools when their
            // respective conditions are met.
            std::set<std::string> allowed_tools;
            for (const auto& t : tools_.tools()) {
                bool include = true;
                if (cmake_tool_names.count(t.name) && !has_cmake_project())
                    include = false;
                if (t.name == "run_bash" && !bash_enabled_)
                    include = false;
                if (include)
                    allowed_tools.insert(t.name);
            }

            json payload = {{"model", model_},
                {"messages", conversation_.build_openai_payload(effective_prompt)},
                {"tools", tools_.to_openai_tools(&allowed_tools)},
                {"stream", true}};
            if (!reasoning_effort_.empty())
                payload["reasoning_effort"] = reasoning_effort_;
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
                conversation_.truncate_conversation(turn_snapshot);
                auto msg = stream_result.error();
                auto raw = client_.last_raw_response();
                if (!raw.empty()) {
                    msg += " | raw: " + raw.substr(0, 500);
                }
                return std::unexpected(std::move(msg));
            }

            if (stream_errored && content.empty()) {
                // Stream error with no content — roll back only this turn
                conversation_.truncate_conversation(turn_snapshot);
                return std::unexpected(stream_error);
            }

            auto calls = tool_acc.finalize();
            if (!calls.empty()) {
                auto msg_id = conversation_.add_assistant("", reasoning, calls);

                if (*cancelled_) {
                    // User cancelled — roll back only this turn
                    conversation_.truncate_conversation(turn_snapshot);
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
                                conversation_.truncate_conversation(turn_snapshot);
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
                            conversation_.add_tool(msg_id, call.id, result);
                        }
                    } else {
                        // Parallel execution for read-only tools
                        // Launch ALL tools first, then collect ALL results.
                        // This ensures no futures are abandoned if cancellation
                        // happens mid-collection — abandoned futures would block
                        // in their destructor and leak background tasks into the
                        // next turn.
                        std::vector<std::future<Result<std::string>>> futures;
                        futures.reserve(calls.size());
                        for (size_t i = 0; i < calls.size(); i++) {
                            futures.push_back(std::async(std::launch::async,
                                [&, i] {
                                    return tools_.execute(calls[i].name, calls[i].arguments);
                                }));
                        }

                        // Collect all tool results before checking cancellation,
                        // so no future is abandoned.
                        std::vector<Result<std::string>> results;
                        results.reserve(calls.size());
                        for (auto& f : futures) {
                            try {
                                results.push_back(f.get());
                            } catch (const std::exception& e) {
                                results.push_back(
                                    std::unexpected(std::string(e.what())));
                            }
                        }

                        if (*cancelled_) {
                            conversation_.truncate_conversation(turn_snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }

                        for (size_t i = 0; i < calls.size(); i++) {
                            if (output_cb_) {
                                output_cb_(
                                    "\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments +
                                        ")",
                                    OutputType::ToolInvocation);
                            }
                            auto result = results[i] ? *results[i] : results[i].error();
                            conversation_.add_tool(msg_id, calls[i].id, result);
                        }
                    }
                } else {
                    for (const auto& call : calls) {
                        if (*cancelled_) {
                            conversation_.truncate_conversation(turn_snapshot);
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
                        conversation_.add_tool(msg_id, call.id, result);
                    }
                }
                continue;
            }

            // No tool calls — content response.
            conversation_.add_assistant(content, reasoning);
            last_content = content;
            last_reasoning = reasoning;
            produced_content = true;
            break;
        }
    } catch (const std::exception& e) {
        conversation_.truncate_conversation(turn_snapshot);
        return std::unexpected(std::string(e.what()));
    }

    if (!produced_content) {
        std::string msg = "Tool call budget exhausted (" +
            std::to_string(max_iterations_) + " iterations).";
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
            // Compaction failed — log but don't fail the overall turn
            if (output_cb_)
                output_cb_("compact() failed: " + compact_result.error(), OutputType::ToolInvocation);
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
    std::string summary_prompt = "Please provide a comprehensive summary of the following conversation. "
        "Preserve all important context, decisions, code changes, file paths, "
        "and outstanding tasks. Be detailed enough that the conversation can "
        "continue seamlessly from this summary.\n\n---\n";

    for (const auto& msg : msgs) {
        if (msg.role == "system") continue; // skip system messages

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
    json payload = {
        {"model", model_},
        {"messages", json::array({
            {{"role", "system"}, {"content", sanitize_utf8(system_prompt_
                + (has_cmake_project() ? std::string(Config::CMAKE_PROMPT_SNIPPET) : ""))}},
            {{"role", "user"}, {"content", sanitize_utf8(summary_prompt)}}
        })},
        {"tools", tools_.to_openai_tools()},
        {"stream", true}
    };
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
        .on_error = [&](const std::string& err) {
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
    last_usage_ = Usage{};  // reset so UI falls back to conversation estimate

    if (output_cb_)
        output_cb_("(conversation compacted — summary follows)\n\n" + summary,
            OutputType::Content);

   return {};
}

// ===================================================================
// MCP server management
// ===================================================================

Result<void> ChatSession::start_mcp_server(const McpEndpoint& config) {
    auto result = mcp_registry_.start_server(config);
    if (!result) {
        return std::unexpected(
            std::string("Failed to start MCP server '") + config.name +
            "': " + result.error());
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
    if (*cancelled_) return;  // honour cancellation token
    conversation_.clear();
    last_usage_ = Usage{};  // reset so context pct falls back to estimate(0)
}
