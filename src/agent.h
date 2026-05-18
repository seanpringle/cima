#pragma once

#include "gui_app.h"
#include "session_data.h"
#include <memory>

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
};

struct SubAgent : Agent {
    std::string subagent_name;    // mapped name from SubagentConfig
    bool read_only_tools = false; // from SubagentConfig.read_only
};

struct PrimaryAgent : Agent {
    bool bash_enabled = false; // run_bash tool enabled for this tab

    // MCP: per-server enabled state (persisted in assistant_data.json)
    std::map<std::string, bool> mcp_enabled;

    // MCP: per-server error message (transient, not persisted)
    std::map<std::string, std::string> mcp_error;

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
