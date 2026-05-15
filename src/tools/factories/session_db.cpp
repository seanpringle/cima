#include "session_db.h"
#include "tools.h"

Tool make_query_session_tool(SessionDB& db) {
    Tool t;
    t.name = "query_session";
    t.description =
        "Execute a SQL query against the session's in-memory SQLite database. "
        "The database is created fresh for each chat session. "
        "Use this to store, organize, and retrieve structured data across tool calls.\n\n"
        "The conversation history is stored in the `messages` table:\n"
        "  messages  — one row per message (user or assistant)\n"
        "    Columns: id, role, content, reasoning_content, tool_data,\n"
        "             suggested_retention\n"
        "    Assistant messages with tool calls store them in `tool_data`\n"
        "    (a JSON array — see format below). Tool results are filled into\n"
        "    the same array as `result` fields.\n\n"
        "  `tool_data` JSON format:\n"
        "    [{\"id\":\"call_A\", \"type\":\"function\",\n"
        "      \"function\":{\"name\":\"list_files\", \"arguments\":\"{\\\"path\\\":\\\".\\\"}\"},\n"
        "      \"result\":\"file1.txt\\nfile2.txt\"}]\n\n"
        "  `suggested_retention` is advisory — 'preserve' or 'droppable'.\n"
        "  Tool results are tagged 'droppable' by default.\n\n"
        "**Key insight: You can modify your own conversation history.**\n"
        "Whatever you write to the `messages` table is what the next API call will see.\n"
        "You curate your own context window.\n\n"
        "Examples:\n"
        "  - SELECT to inspect history\n"
        "  - UPDATE to compress tool results:\n"
        "      UPDATE messages SET tool_data = ... WHERE id = <assistant_id>\n"
        "  - DELETE to prune: DELETE FROM messages WHERE id = <assistant_id>\n"
        "    (self-contained — no orphan risk)\n"
        "  - INSERT to inject summaries or memory records\n\n"
        "**Constraint:** Keep the `messages` table schema as-is —\n"
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
