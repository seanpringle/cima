#include "group_channel.h"
#include "tools.h"

#include <sstream>

// ── post_message ─────────────────────────────────────────────────────

static Tool make_post_message_tool(GroupChannel& channel) {
    Tool t;
    t.name = "post_message";
    t.description =
        "Post a message to the group channel visible to all agents and the user. "
        "The system automatically scans the message for @mentions of "
        "other agent names, @user, and @everyone, and records them as tags. "
        "Tagged agents will be notified to check the channel.\n\n"
        "Parameters:\n"
        "  message  — markdown body of the message\n\n"
        "Returns: {\"id\": <int>} — the message id.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"message",
                {{"type", "string"},
                    {"description", "Markdown body of the message"}}}}},
        {"required", {"message"}}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        auto message = args.value("message", std::string());
        if (message.empty()) {
            return std::unexpected("message is required");
        }
        // The from field will be filled by the calling session's name.
        // But we don't know the caller's name here... We'll need to set it
        // from the calling session. For now, use "unknown" — the caller
        // will override this via ChatSession.
        int id = channel.post_message("unknown", message);
        json result = {{"id", id}};
        return result.dump();
    };
    return t;
}

// ── read_new_messages ───────────────────────────────────────────────

static Tool make_read_new_messages_tool(GroupChannel& channel) {
    Tool t;
    t.name = "read_new_messages";
    t.description =
        "Return messages that your agent has not yet read, and advance "
        "your per-agent cursor to the latest message. "
        "On first call, returns all messages in the channel.\n"
        "Returns an array of objects: [{id, from, message, tags}, ...] "
        "ordered by id (oldest first).\n\n"
        "No parameters needed — the cursor is tracked server-side per agent.\n\n"
        "There is no need to manually track message ids — the cursor is "
        "per-agent and server-side, so you will never miss messages that "
        "arrived between calls.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        (void)args;
        // The agent name will be injected by the ChatSession wrapper.
        // For now use "unknown" — the caller overrides this.
        auto msgs = channel.read_new_messages("unknown");
        json arr = json::array();
        for (const auto& m : msgs) {
            json tags = json::array();
            for (const auto& tag : m.tags)
                tags.push_back(tag);
            arr.push_back({{"id", m.id},
                {"from", m.from},
                {"message", m.message},
                {"tags", tags}});
        }
        return arr.dump(2);
    };
    return t;
}

// ── Factory ──────────────────────────────────────────────────────────

std::vector<Tool> make_group_channel_tools(GroupChannel& channel) {
    std::vector<Tool> tools;
    tools.push_back(make_read_new_messages_tool(channel));
    tools.push_back(make_post_message_tool(channel));
    return tools;
}
