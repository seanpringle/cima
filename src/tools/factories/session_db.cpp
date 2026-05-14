#include "session_db.h"
#include "tools.h"

Tool make_query_session_tool(SessionDB& db) {
    Tool t;
    t.name = "query_session";
    t.description =
        "Execute a SQL query against the session's in-memory SQLite database. "
        "The database is created fresh for each chat session. "
        "Use this to store, organize, and retrieve structured data across tool calls.\n\n"
        "The conversation history is stored in built-in tables:\n"
        "  messages  — one row per message\n"
        "    Columns: id, seq, role, content, reasoning_content, tool_call_id,\n"
        "             retention, created_at\n"
        "    The `retention` column is either 'preserve' (keep this message) or\n"
        "    'droppable' (safe to compress/summarize — tool results are tagged\n"
        "    'droppable' by default).\n"
        "  tool_calls — one row per tool_call within an assistant message\n"
        "    Columns: id, message_id, call_index, tool_call_id, name, arguments\n"
        "  metadata   — key-value store for session state (context usage, budgets, etc.)\n"
        "    Columns: key TEXT PRIMARY KEY, value TEXT\n\n"
        "**Key insight: You can modify your own conversation history.**\n"
        "Whatever you write to the `messages` table is what the next API call will see.\n"
        "You curate your own context window.\n\n"
        "Examples:\n"
        "  - SELECT to inspect history\n"
        "  - UPDATE to compress large tool results (target retention='droppable' rows)\n"
        "  - DELETE to prune old entries\n"
        "  - INSERT to inject summaries or memory records\n\n"
        "**Constraint:** Keep the `messages` and `tool_calls` table schemas as-is —\n"
        "the tool that builds your next API request depends on these columns.\n"
        "You can add your own tables freely.\n\n"
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
