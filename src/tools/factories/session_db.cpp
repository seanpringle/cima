#include "session_db.h"
#include "tools.h"

Tool make_query_session_tool(SessionDB& db) {
    Tool t;
    t.name = "query_session";
    t.description =
        "Execute a SQL query against the session's in-memory SQLite database. "
        "The database is created fresh for each chat session. "
        "Use this to store, organize, and retrieve structured data across tool calls.\n\n"
        "The conversation history is stored in two built-in tables:\n"
        "  messages  — one row per message (columns: id, seq, role, content,\n"
        "              reasoning_content, tool_call_id, retention, created_at)\n"
        "  tool_calls — one row per tool_call within an assistant message\n"
        "              (columns: id, message_id, call_index, tool_call_id, name, arguments)\n\n"
        "You can read, write, and manage these tables to control your own context.\n"
        "For example: SELECT to inspect history, UPDATE to compress large tool results,\n"
        "DELETE to prune old entries, INSERT to inject summaries.\n"
        "The next API call will read from the modified tables.\n\n"
        "Supports all SQL statements: CREATE TABLE, INSERT, SELECT, UPDATE, DELETE, etc.\n\n"
        "Results are returned as JSON:\n"
        "  - SELECT returns an array of row objects: [{\"col\": val}, ...]\n"
        "  - INSERT/UPDATE/DELETE returns: {\"rows_affected\": N}\n"
        "  - DDL returns: {\"ok\": true}";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"query",
                {{"type", "string"},
                    {"description",
                        "SQL query string to execute (max 100 KB). "
                        "Can be any valid SQLite statement."}}}}},
        {"required", {"query"}}};
    t.execute = [&db](const json& args) -> Result<std::string> {
        auto sql = args.value("query", std::string());
        if (sql.empty()) {
            return std::unexpected(std::string("query is required"));
        }
        return db.execute(sql);
    };
    return t;
}
