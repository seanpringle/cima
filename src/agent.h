#pragma once

#include "gui_app.h"
#include "session_data.h"

#include <memory>

struct lua_State;

struct Agent {
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<AsyncChatState> chat_state;
    ChatUIState ui_state;
    int id = 0;
    std::string title;            // Culture ship name (display label)
    std::string model_name;       // actual model name (shown in dropdown)
    std::string provider_name;    // which provider this tab belongs to
    std::string reasoning_effort; // per-tab reasoning effort override
    std::string git_branch;

    void cancel_and_wait();

    /// Poll the async chat future; if ready, get the result (or error),
    /// set chat_state->running = false, and return the result.
    /// Returns std::nullopt if the future is not yet ready.
    std::optional<Result<ChatResult>> check_finished();

    /// Launch an async run_once(input) in a background thread.
    void start_chat(std::string input);

    /// Drain pending streaming output from the async thread into ui_state.
    void drain_pending();

    /// If models have been fetched and not yet validated, auto-select the
    /// first available model if the current model is not in the list.
    void validate_current_model();

    /// Poll for completion of an in-flight compact operation.
    void poll_compact();

    /// Poll for completion of an in-flight clear operation.
    void poll_clear();
};

struct SubAgent : Agent {
    std::string subagent_name;    // mapped name from SubagentConfig
    bool read_only_tools = false; // from SubagentConfig.read_only
};

/// Editing state for the session snippets CRUD UI (Config tab).
struct SnippetEditState {
    bool active = false;                  // true when editing/add is open
    std::string original_name;            // empty = new snippet, non-empty = editing existing
    std::string name_buf;                 // name input buffer
    std::string content_buf;              // content input buffer
    std::string error;                    // validation error to display
};

/// Editing state for the session custom commands CRUD UI (Config tab).
struct CmdEditState {
    bool active = false;           // true when editing/add is open
    std::string original_name;     // empty = new, non-empty = editing existing
    std::string name_buf;          // name input buffer
    std::string desc_buf;          // description input buffer
    std::string command_buf;       // command input buffer
    std::string error;             // validation error to display
};

struct PrimaryAgent : Agent {
    bool bash_enabled = false; // run_bash tool enabled for this tab
    bool cmake_enabled = false; // cmake tools enabled for this tab
    SnippetEditState snippet_edit; // session snippets CRUD editing state
    CmdEditState cmd_edit; // session custom commands CRUD editing state

    // MCP: per-server enabled state (persisted in assistant_data.json)
    std::map<std::string, bool> mcp_enabled;

    // Custom cmd_tools: per-tool enabled state (persisted)
    std::map<std::string, bool> cmd_tools_enabled;

    // Per-tool gate overrides (persisted): tool name -> enabled
    std::map<std::string, bool> tool_gates;

    // MCP: per-server error message (transient, not persisted)
    std::map<std::string, std::string> mcp_error;

    std::shared_ptr<lua_State> lua_state_;   // persistent Lua VM (nullptr = disabled)
    std::shared_ptr<std::mutex> lua_mutex_;  // mutex for thread-safe Lua access

    std::vector<SubAgent> subagents;

    SessionData& session_data;

    PrimaryAgent(SessionData&);
    ~PrimaryAgent();

    Result<SubAgent*> subagent_by_name(const std::string& name);
    void cancel_running_chats();

  private:
    void init_defaults();
    void create_chat_session();
    void restore_session_data();
    void create_subagents();
    void register_subagent_tools();
};
