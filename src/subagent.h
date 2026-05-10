#pragma once

#include "config.h"
#include "types.h"

#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// SubagentDef — configuration for a named subagent
// ---------------------------------------------------------------------------

struct SubagentDef {
    std::string name;            // "explore", "general"
    std::string display_name;    // human-readable label
    std::string system_prompt;   // system prompt for the subagent
    std::set<std::string> allowed_tools; // empty = use default based on mode
    std::string api_base;        // empty = inherit from primary
    std::string api_key;         // empty = inherit from primary
    std::string model;           // empty = inherit from primary
};

/// Return the built-in subagent definitions.
std::vector<SubagentDef> builtin_subagents();

/// Look up a built-in subagent by name. Returns nullptr if not found.
const SubagentDef* find_builtin_subagent(const std::string& name);
