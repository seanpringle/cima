#pragma once

#include "config.h"
#include "types.h"

#include <string>
#include <vector>

/// Consolidated assistant data that replaces the previous multi-file layout.
///
/// Previously each assistant tab stored separate files:
///   <name>.messages.json  — Conversation
///   <name>.log            — ChatUIState display log (JSON Lines)
///   <name>.plan.json      — PlanBoard
///   <name>.notes.json     — Notes
///
/// Now all of these are combined into a single <name>.json file.
struct AssistantData {
    int version = 1;
    std::string name;               // agent's Culture ship name (matches filename stem)
    std::string provider_name;      // which provider this tab belongs to
    std::string model;              // model name for this assistant
    std::string reasoning_effort;   // reasoning effort override
    json conversation;              // serialized Conversation (array of message objects)
    json chat_log;                  // serialized chat log entries (array of entry objects)
    json plan;                      // { "plan": "...", "comments": [...] }
    json notes;                     // { "note_name": "body", ... }

    json to_json() const;
    void from_json(const json& j);

    Result<void> save_to_file(const std::string& path) const;
    Result<void> load_from_file(const std::string& path);
};
