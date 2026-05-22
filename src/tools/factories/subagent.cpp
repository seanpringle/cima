#include "chat.h"
#include "tools.h"
#include "agent.h"

// ===================================================================
// Tool: call_subagent
// ===================================================================

Tool make_call_subagent_tool(PrimaryAgent& primary,
    const std::vector<SubagentConfig>& subagent_configs,
    int timeout_sec) {
    Tool t;
    t.name = "call_subagent";

    // Build a description that includes available subagent names
    std::string desc = "Call a subagent by name with a request. The subagent will "
                       "execute the request using its own configured model and tools, "
                       "and return the response.\n\n"
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
    t.timeout_sec = timeout_sec;

    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                 {{"type", "string"}, {"description", "Name of the subagent to invoke"}}},
                {"request",
                    {{"type", "string"},
                        {"description", "The request/prompt to send to the subagent"}}}}},
        {"required", {"name", "request"}}};
    t.execute = [&primary](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto request = args.value("request", std::string());

        if (name.empty()) {
            return std::unexpected("subagent name must not be empty");
        }
        if (request.empty()) {
            return std::unexpected("subagent request must not be empty");
        }

        auto lookup = primary.subagent_by_name(name);
        if (!lookup) {
            return std::unexpected("subagent name unknown: " + name);
        }

        SubAgent& subagent = **lookup;

        // Check if subagent is already running
        if (subagent.chat_state->running) {
            return std::unexpected("subagent \"" + name +
                                   "\" is already running — wait for it to finish");
        }

        // Reset subagent state: clear conversation and UI entries
        subagent.session->conversation().clear();
        subagent.ui_state.entries.clear();
        subagent.ui_state.next_seq = 1;

        // Push the primary agent's request as a UserText entry in the subagent chat
        subagent.ui_state.entries.push_back(
            {EntryType::UserText, request, false, subagent.ui_state.next_seq++});

        // Run the request
        auto result = subagent.session->run_once(request);
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
