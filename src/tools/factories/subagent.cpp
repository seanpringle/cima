#include "chat.h"
#include "tools.h"
#include "agent.h"

// ===================================================================
// Tool: call_subagent
// ===================================================================

Tool make_call_subagent_tool(
    PrimaryAgent& primary, const std::vector<SubagentConfig>& subagent_configs, int timeout_sec) {
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

    t.permission = ToolPermission::Write; // serialise to prevent concurrent subagent runs
    t.timeout_sec = timeout_sec;

    t.parameters = {{"type", "object"},
        {"properties",
            {{"name", {{"type", "string"}, {"description", "Name of the subagent to invoke"}}},
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

        // Set up a promise/future pair BEFORE claiming the subagent, so that
        // cancel_and_wait() (called on shutdown) always sees a valid future
        // if running is true — closing the race between the two checks.
        auto promise = std::make_shared<std::promise<Result<ChatResult>>>();
        subagent.chat_state->future = promise->get_future();

        // Atomically claim the subagent (guard against concurrent tool-initiated runs).
        // compare_exchange closes the check-to-set race between two tool calls.
        bool expected = false;
        if (!subagent.chat_state->running.compare_exchange_strong(expected, true)) {
            // Already running — reset the future we just set (nobody can be waiting
            // on it because running was false).
            subagent.chat_state->future = std::future<Result<ChatResult>>();
            return std::unexpected(
                "subagent \"" + name + "\" is already running — wait for it to finish");
        }

        // Reset subagent state: clear conversation, pending output, UI entries,
        // and reset cancellation token (so a cancelled-but-not-reused subagent
        // gets a fresh start).
        subagent.session->conversation().clear();
        {
            std::lock_guard<std::mutex> lock(subagent.chat_state->mutex);
            subagent.chat_state->pending.clear();
        }
        *subagent.chat_state->cancelled = false;
        subagent.ui_state.entries.clear();

        // Push the primary agent's request as a UserText entry in the subagent chat
        subagent.ui_state.entries.push_back({EntryType::UserText, request, false});

        // Run the request (may throw only in truly exceptional cases — bad_alloc etc.)
        Result<ChatResult> result;
        try {
            result = subagent.session->run_once(request);
        } catch (const std::exception& e) {
            subagent.chat_state->running = false;
            promise->set_value(std::unexpected(std::string(e.what())));
            return std::unexpected(std::string("subagent error: ") + e.what());
        }

        // Build response string while result is still alive
        if (!result) {
            std::string err = result.error();
            subagent.chat_state->running = false;
            promise->set_value(std::unexpected(std::move(err)));
            return std::unexpected("subagent error: " + result.error());
        }

        std::string response;
        if (!result->reasoning.empty()) {
            response += result->reasoning + "\n";
        }
        response += result->content;

        // Cleanup: clear the running flag, then fulfil the promise so that
        // cancel_and_wait() (if waiting) can proceed.
        subagent.chat_state->running = false;
        promise->set_value(std::move(result));

        return response;
    };
    return t;
}
