#include "session_db.h"
#include "tools.h"

Tool make_query_session_tool(SessionDB& db) {
    Tool t;
    t.name = "query_session";
    t.description =
        "Execute a SQL query against the session's in-memory SQLite database. "
        "The database is created fresh for each chat session. "
        "Use this to store, organize, and retrieve structured data across tool calls.\n\n"
        "This is scratch space — create your own tables, insert data, and query "
        "results freely. No built-in tables are created.\n\n"
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
