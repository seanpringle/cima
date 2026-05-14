#include "group_channel.h"
#include "tools.h"

#include <sstream>

// ── list_all_messages ────────────────────────────────────────────────

static Tool make_list_all_messages_tool(GroupChannel& channel) {
    Tool t;
    t.name = "list_all_messages";
    t.description =
        "List all messages in the group channel. "
        "Returns an array of objects: [{id, from, summary, tags}, ...] "
        "ordered by id (oldest first). "
        "If there are more than 200 messages, only the last 200 are returned. "
        "Use list_messages_since() to paginate.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        (void)args;
        auto msgs = channel.list_all_messages();
        json arr = json::array();
        for (const auto& m : msgs) {
            json tags = json::array();
            for (const auto& tag : m.tags)
                tags.push_back(tag);
            arr.push_back({{"id", m.id},
                {"from", m.from},
                {"summary", m.summary},
                {"tags", tags}});
        }
        return arr.dump(2);
    };
    return t;
}

// ── list_messages_since ──────────────────────────────────────────────

static Tool make_list_messages_since_tool(GroupChannel& channel) {
    Tool t;
    t.name = "list_messages_since";
    t.description =
        "List messages in the group channel posted after the given id. "
        "Returns an array of objects: [{id, from, summary, tags}, ...] "
        "ordered by id (oldest first). "
        "Use this to poll for new messages without re-reading everything.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"id",
                {{"type", "integer"},
                    {"description",
                        "Return only messages with id greater than this value"}}}}},
        {"required", {"id"}}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        auto id = args.value("id", 0);
        auto msgs = channel.list_messages_since(id);
        json arr = json::array();
        for (const auto& m : msgs) {
            json tags = json::array();
            for (const auto& tag : m.tags)
                tags.push_back(tag);
            arr.push_back({{"id", m.id},
                {"from", m.from},
                {"summary", m.summary},
                {"tags", tags}});
        }
        return arr.dump(2);
    };
    return t;
}

// ── read_message_body ────────────────────────────────────────────────

static Tool make_read_message_body_tool(GroupChannel& channel) {
    Tool t;
    t.name = "read_message_body";
    t.description =
        "Read the full detailed body of a specific message in the group channel "
        "by its id. Returns a markdown string, or an error if the message "
        "does not exist.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"id",
                {{"type", "integer"},
                    {"description", "The id of the message to read"}}}}},
        {"required", {"id"}}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        auto id = args.value("id", 0);
        auto msg = channel.read_message_body(id);
        if (!msg)
            return std::unexpected("message not found: " + std::to_string(id));
        return msg->body;
    };
    return t;
}

// ── post_message ─────────────────────────────────────────────────────

static Tool make_post_message_tool(GroupChannel& channel) {
    Tool t;
    t.name = "post_message";
    t.description =
        "Post a message to the group channel visible to all agents and the user. "
        "The system automatically scans the summary and body for @mentions of "
        "other agent names, @user, and @everyone, and records them as tags. "
        "Tagged agents will be notified to check the channel.\n\n"
        "Parameters:\n"
        "  short_summary  — brief markdown summary (shown in the channel list view)\n"
        "  longer_body    — detailed markdown body (read with read_message_body)\n\n"
        "Returns: {\"id\": <int>} — the message id, which can be used with "
        "read_message_body() or list_messages_since().";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"short_summary",
                 {{"type", "string"},
                     {"description",
                         "Short markdown summary visible in the channel list"}}},
                {"longer_body",
                    {{"type", "string"},
                        {"description",
                            "Long detailed markdown body of the message"}}}}},
        {"required", {"short_summary", "longer_body"}}};
    t.execute = [&channel](const json& args) -> Result<std::string> {
        auto summary = args.value("short_summary", std::string());
        auto body = args.value("longer_body", std::string());
        if (summary.empty()) {
            return std::unexpected("short_summary is required");
        }
        if (body.empty()) {
            return std::unexpected("longer_body is required");
        }
        // The from field will be filled by the calling session's name.
        // But we don't know the caller's name here... We'll need to set it
        // from the calling session. For now, use "unknown" — the caller
        // will override this via ChatSession.
        int id = channel.post_message("unknown", summary, body);
        json result = {{"id", id}};
        return result.dump();
    };
    return t;
}

// ── Factory ──────────────────────────────────────────────────────────

std::vector<Tool> make_group_channel_tools(GroupChannel& channel) {
    std::vector<Tool> tools;
    tools.push_back(make_list_all_messages_tool(channel));
    tools.push_back(make_list_messages_since_tool(channel));
    tools.push_back(make_read_message_body_tool(channel));
    tools.push_back(make_post_message_tool(channel));
    return tools;
}
