#pragma once

#include "client.h"
#include "config.h"
#include "conversation.h"
#include "mcp/mcp_registry.h"
#include "plan.h"
#include "tools.h"
#include "types.h"

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

struct ChatResult {
    std::string content;
    std::string reasoning;
};

enum class OutputType { Reasoning, Content, ToolInvocation };

using OutputCallback = std::function<void(const std::string& text, OutputType type)>;

class ChatSession {
  public:
    explicit ChatSession(
        const Config& config, const Provider& provider, CancellationToken cancelled = nullptr);

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;
    ChatSession(ChatSession&&) = delete;
    ChatSession& operator=(ChatSession&&) = delete;

    /// Create a subagent session with restricted tools and a simpler system prompt.
    /// If read_only is true, write tools (file, git) are excluded.
    /// If primary_plan is non-null, the subagent's read_plan tool reads from
    /// that PlanBoard (typically the primary agent's plan) instead of its own.
    static std::unique_ptr<ChatSession> create_subagent(const Config& config,
        const Provider& provider,
        bool read_only,
        CancellationToken cancelled);

    Result<ChatResult> run_once(const std::string& user_input);
    void set_model(const std::string& m) { model_ = m; }
    const std::string& model() const { return model_; }
    void set_reasoning_effort(const std::string& v) { reasoning_effort_ = v; }
    const std::string& reasoning_effort() const { return reasoning_effort_; }
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }
    const Usage& last_usage() const { return last_usage_; }

    // Each session has its own PlanBoard (not shared across agents).
    PlanBoard& plan() { return ::plan; }
    const PlanBoard& plan() const { return ::plan; }

    // API connection info (for creating temporary clients in background tasks).
    const std::string& api_base() const { return api_base_; }
    const std::string& api_key() const { return api_key_; }

    // Each session has its own conversation history.
    Conversation& conversation() { return conversation_; }
    const Conversation& conversation() const { return conversation_; }

    // Expose the underlying client so the GUI can call fetch_models() etc.
    ChatClient& client_for_models() { return client_; }

    // Return the current safe directory (workspace) path for this session.
    const std::string& safe_dir() const { return *safe_dir_; }

    /// safe_dir is locked to the cwd at construction time and cannot change.
    /// (set_safe_dir removed)

    /// Compact the conversation by asking the LLM to summarise.
    /// Replaces the entire history with a single summary message.
    Result<void> compact();

    /// Clear all messages from the conversation and reset context tracking.
    void clear();

    /// Compute context usage percentage (0-100).
    int context_usage_percent() const;

    /// Set/Get the agent's Culture ship name.
    void set_agent_name(const std::string& name) { agent_name_ = name; }
    const std::string& agent_name() const { return agent_name_; }

    /// True when a CMakeLists.txt exists in the workspace directory.
    bool has_cmake_project() const;

    /// Enable/disable the run_bash tool for this session.
    void set_bash_enabled(bool v) { bash_enabled_ = v; }
    bool bash_enabled() const { return bash_enabled_; }

    /// Provider name this session belongs to.
    const std::string& provider_name() const { return provider_name_; }

    /// Update the session to use a different provider at runtime.
    /// Updates api_base, api_key, model, reasoning_effort, and the underlying ChatClient.
    void set_provider(const Provider& provider);

    // ── MCP server management ──

    /// Start an MCP server from its endpoint configuration.
    /// Discovers tools and registers them in the ToolRegistry with "mcp_" prefix.
    Result<void> start_mcp_server(const McpEndpoint& config);

    /// Stop a running MCP server by name.
    /// Removes its tools from the ToolRegistry.
    void stop_mcp_server(const std::string& name);

    /// Access the underlying MCP registry (for GUI status queries).
    McpRegistry& mcp_registry() { return mcp_registry_; }
    const McpRegistry& mcp_registry() const { return mcp_registry_; }

    /// Register the call_subagent tool in this session's tool registry.
    /// The lookup callable receives a subagent name and returns its ChatSession*.
    /// clear_ui resets the subagent's UI state; push_entry pushes a UserText entry
    /// (the primary agent's request).  subagent_configs are used to build the
    /// tool description with available names.
    void register_call_subagent_tool(
        PrimaryAgent& primary, const std::vector<SubagentConfig>& subagent_configs = {}) {
        tools_.add(make_call_subagent_tool(primary, subagent_configs));
    }

    /// Access the tool registry (for testing and GUI).
    const ToolRegistry& tools_for_testing() const { return tools_; }

  private:
    // ── Refactored run_once() helpers ──

    /// Result of a single streaming turn (content + reasoning + tool calls).
    struct StreamResult {
        std::string content;
        std::string reasoning;
        std::vector<ToolCall> calls;
    };

    /// Discover and cache the model's context limit (one-time per model/endpoint).
    void discover_context_limit();

    /// Build the effective system prompt with conditional CMake/MCP sections.
    std::string build_effective_prompt() const;

    /// Return tool names permitted for this iteration (CMake/bash conditional).
    std::set<std::string> filter_allowed_tools() const;

    /// Assemble the OpenAI-compatible request payload.
    json build_payload(const std::set<std::string>& allowed_tools) const;

    /// Stream a chat completion and return the accumulated content/reasoning/tool_calls.
    /// On error the caller handles rollback.
    Result<StreamResult> stream_chat(const json& payload);

    /// Execute tool calls for a single assistant message — serial for write tools,
    /// parallel for read-only, single for one call.
    /// Results are added to the conversation via add_tool().
    /// Returns an error only if the user cancels (caller should rollback).
    Result<void> execute_tool_calls(
        int64_t msg_id, const std::vector<ToolCall>& calls, int remaining_iters);
    std::string model_;
    std::string reasoning_effort_;
    std::string agent_name_;
    std::string provider_name_;
    std::shared_ptr<std::string> safe_dir_;
    std::string api_base_;     // API base URL (for creating temp clients)
    std::string api_key_;      // API key for authentication
    int max_iterations_ = 100; // overridden by config.max_tool_iterations
    std::shared_ptr<FileModifiedCallback> file_modified_cb_;
    std::string system_prompt_;
    Conversation conversation_;
    ChatClient client_;
    CancellationToken cancelled_;
    ToolRegistry tools_;
    OutputCallback output_cb_;
    Usage last_usage_;
    int context_limit_ = 300000; // discovered from API, falls back to Config
    bool context_limit_discovered_ = false;
    bool bash_enabled_ = false;
    McpRegistry mcp_registry_;
};
