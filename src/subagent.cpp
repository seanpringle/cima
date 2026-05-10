#include "subagent.h"

std::vector<SubagentDef> builtin_subagents() {
    std::vector<SubagentDef> agents;

    // ── explore: read-only research agent ──
    agents.push_back({
        .name = "explore",
        .display_name = "Explore",
        .system_prompt =
            "You are a research assistant. "
            "Use read-only tools (list_files, read_file, read_file_lines, "
            "grep_files, project_tree, web_search, web_fetch, git_status, "
            "git_diff, git_log) to gather information. "
            "Be thorough, explore multiple angles, and cite your sources. "
            "You cannot modify files, execute shell commands, or make git "
            "changes. "
            "Output a concise summary of your findings.",
        .allowed_tools = {}, // filled dynamically from read-only tools
        .api_base = "",
        .api_key = "",
        .model = "",
    });

    // ── general: full-access assistant ──
    agents.push_back({
        .name = "general",
        .display_name = "General",
        .system_prompt =
            "You are a general-purpose assistant. "
            "You have access to all tools except subagent delegation. "
            "Answer questions, solve problems, and complete tasks as requested. "
            "Be concise and direct.",
        .allowed_tools = {}, // all tools except run_subagent (filled by caller)
        .api_base = "",
        .api_key = "",
        .model = "",
    });

    return agents;
}

const SubagentDef* find_builtin_subagent(const std::string& name) {
    static const auto agents = builtin_subagents();
    for (const auto& a : agents) {
        if (a.name == name)
            return &a;
    }
    return nullptr;
}
