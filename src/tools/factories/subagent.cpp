#include "chat.h"
#include "tools.h"

// ===================================================================
// Tool: call_subagent
// ===================================================================

Tool make_call_subagent_tool(SubagentLookup lookup, SubagentRunningCheck is_running,
    SubagentClearUI clear_ui,
    const std::vector<SubagentConfig>& subagent_configs) {
    Tool t;
    t.name = "call_subagent";

    // Build a description that includes available subagent names
    std::string desc = "Call a subagent by name with a request. The subagent will "
                       "execute the request using its own configured model and tools, "
                       "and return the response. The subagent's conversation appears "
                       "in its own tab in the UI.\n\n"
                       "Available subagents:\n";
    if (subagent_configs.empty()) {
        desc += "  (none configured — add a \"subagents\" array to cima.json)\n";
    } else {
        for (const auto& sa : subagent_configs) {
            desc += "  - \"" + sa.name + "\"";
            if (!sa.description.empty()) {
                desc += ": " + sa.description;
            }
            desc += "\n";
        }
    }
    t.description = desc;

    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 600; // 10 minutes
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                 {{"type", "string"}, {"description", "Name of the subagent to invoke"}}},
                {"request",
                    {{"type", "string"},
                        {"description", "The request/prompt to send to the subagent"}}}}},
        {"required", {"name", "request"}}};
    t.execute = [lookup = std::move(lookup), is_running = std::move(is_running),
                    clear_ui = std::move(clear_ui)](
                    const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto request = args.value("request", std::string());

        if (name.empty()) {
            return std::unexpected("subagent name must not be empty");
        }
        if (request.empty()) {
            return std::unexpected("subagent request must not be empty");
        }

        // Check if subagent is already running
        if (is_running(name)) {
            return std::unexpected("subagent \"" + name +
                                   "\" is already running — wait for it to finish");
        }

        // Look up the subagent session
        auto* session = lookup(name);
        if (!session) {
            return std::unexpected("subagent \"" + name + "\" not found");
        }

        // Reset subagent state: clear conversation and UI entries
        session->conversation().clear();
        if (clear_ui) {
            clear_ui(name);
        }

        // Run the request
        auto result = session->run_once(request);
        if (!result) {
            return std::unexpected("subagent error: " + result.error());
        }

        std::string response;
        if (!result->reasoning.empty()) {
            response += result->reasoning + "\n";
        }
        response += result->content;
        return response;
    };
    return t;
}
