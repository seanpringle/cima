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

    json to_json() const;
    void from_json(const json& j);

    Result<void> save_to_file(const std::string& path) const;
    Result<void> load_from_file(const std::string& path);
};
