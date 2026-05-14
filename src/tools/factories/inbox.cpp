#include "inbox.h"
#include "tools.h"

#include <sstream>

// ── send_message ─────────────────────────────────────────────────────

static Tool make_send_message_tool(Inbox& inbox) {
    Tool t;
    t.name = "send_message";
    t.description =
        "Send a message to another agent's inbox. The recipient must be a "
        "registered agent name.\n\n"
        "Parameters:\n"
        "  recipient  — name of the recipient agent\n"
        "  message    — markdown body of the message\n\n"
        "Returns: \"delivered\" or \"no such recipient\".";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"recipient",
                 {{"type", "string"},
                     {"description",
                         "Name of the recipient agent"}}},
                {"message",
                    {{"type", "string"},
                        {"description",
                            "Markdown body of the message"}}}}},
        {"required", {"recipient", "message"}}};
    t.execute = [&inbox](const json& args) -> Result<std::string> {
        auto recipient = args.value("recipient", std::string());
        auto message = args.value("message", std::string());
        if (recipient.empty()) {
            return std::unexpected("recipient is required");
        }
        if (message.empty()) {
            return std::unexpected("message is required");
        }
        return inbox.send_message("unknown", recipient, message);
    };
    return t;
}

// ── next_message ─────────────────────────────────────────────────────

static Tool make_next_message_tool(Inbox& inbox) {
    Tool t;
    t.name = "next_message";
    t.description =
        "Read and remove the next message from your inbox. Messages are "
        "returned in FIFO order (oldest first). If there are no messages "
        "waiting, returns \"no messages\".\n\n"
        "No parameters needed — operates on your own inbox.\n\n"
        "Returns: {\"sender\": name, \"message\": markdown} or \"no messages\".";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&inbox](const json& args) -> Result<std::string> {
        (void)args;
        // The agent name will be injected by the ChatSession wrapper.
        auto msg = inbox.next_message("unknown");
        if (!msg.has_value()) {
            return std::string("\"no messages\"");
        }
        json result = {{"sender", msg->from}, {"message", msg->message}};
        return result.dump();
    };
    return t;
}

// ── Factory ──────────────────────────────────────────────────────────

std::vector<Tool> make_inbox_tools(Inbox& inbox) {
    std::vector<Tool> tools;
    tools.push_back(make_send_message_tool(inbox));
    tools.push_back(make_next_message_tool(inbox));
    return tools;
}
