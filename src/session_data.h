#pragma once

#include "config.h"
#include "types.h"

#include <map>
#include <string>

/// Consolidated session data — the entire on-disk state for one cima session.
///
/// Previously each assistant tab stored separate files and a manifest
/// (state.json) tracked them. Now all of this is a single JSON file:
///   ~/.local/state/cima/<session>.json
///
/// Version 2 is the single-file format (replaces the multi-file layout
/// where a state.json manifest referenced per-tab .json files).
struct SessionData {
    int version = 2;
    std::string last_cwd;         // working directory last used
    std::string provider_name;    // which provider this session belongs to
    std::string model;            // model name
    std::string reasoning_effort; // reasoning effort override
    // workspace_path removed — safe_dir is always the cwd at startup
    json conversation;                       // serialized Conversation (array of message objects)
    json chat_log;                           // serialized chat log entries (array of entry objects)
    json plan;                               // { "plan": "...", "comments": [...] }
    bool bash_enabled = false;               // run_bash tool enabled for this session
    bool cmake_enabled = false;              // cmake tools enabled for this session
    std::map<std::string, bool> mcp_enabled; // per-server MCP enabled state
    std::map<std::string, bool> cmd_tools_enabled;      // per-cmd-tool enabled state
    std::map<std::string, bool> tool_gates;             // per-tool enabled overrides (persisted)
    std::map<std::string, bool> rw_subagent_tool_gates; // read-write subagent gates (persisted)
    std::map<std::string, bool> ro_subagent_tool_gates; // read-only subagent gates (persisted)
    std::map<std::string, std::string> snippets; // session-local snippet overrides (persisted)
    std::map<std::string, CmdToolConfig>
        custom_commands;                         // session-local custom commands (persisted)
    std::vector<McpEndpoint> custom_mcp_servers; // session-local custom MCP servers (persisted)
    std::vector<std::string> input_history;       // per-tab input history (persisted)

    json to_json() const;
    void from_json(const json& j);

    Result<void> save_to_file(const std::string& path) const;
    Result<void> load_from_file(const std::string& path);
};
