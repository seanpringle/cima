#include "session_db.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

SessionDB::SessionDB() {
    int rc = sqlite3_open(":memory:", &db_);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("Failed to open in-memory SQLite database: " + msg);
    }
    init_conversation_tables();
}

SessionDB::~SessionDB() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void SessionDB::init_conversation_tables() {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            seq INTEGER NOT NULL,
            role TEXT NOT NULL,
            content TEXT,
            reasoning_content TEXT DEFAULT '',
            tool_call_id TEXT DEFAULT '',
            retention TEXT DEFAULT 'preserve',
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS tool_calls (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            message_id INTEGER NOT NULL REFERENCES messages(id) ON DELETE CASCADE,
            call_index INTEGER NOT NULL,
            tool_call_id TEXT NOT NULL,
            name TEXT NOT NULL,
            arguments TEXT DEFAULT ''
        );
        CREATE INDEX IF NOT EXISTS idx_messages_seq ON messages(seq);
        CREATE INDEX IF NOT EXISTS idx_tool_calls_msg ON tool_calls(message_id);
    )";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        // Don't throw — the DB is still usable for user tables
    }
}

int64_t SessionDB::claim_seq() {
    return ++next_seq_;
}

int64_t SessionDB::add_user(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t seq = claim_seq();
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO messages (seq, role, content, retention) VALUES (?, 'user', ?, 'preserve')";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, seq);
    sqlite3_bind_text(stmt, 2, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return -1;
    }
    return sqlite3_last_insert_rowid(db_);
}

int64_t SessionDB::add_assistant(const std::string& content,
    const std::string& reasoning,
    const std::vector<ToolCall>& tool_calls) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t seq = claim_seq();

    if (!tool_calls.empty()) {
        // Tool-call assistant message: content = NULL
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO messages (seq, role, content, reasoning_content, retention) "
            "VALUES (?, 'assistant', NULL, ?, 'preserve')";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, seq);
        sqlite3_bind_text(stmt, 2, reasoning.c_str(), static_cast<int>(reasoning.size()), SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            return -1;
        }
        int64_t msg_id = sqlite3_last_insert_rowid(db_);

        // Insert tool calls
        for (size_t i = 0; i < tool_calls.size(); i++) {
            const auto& tc = tool_calls[i];
            sqlite3_stmt* tc_stmt = nullptr;
            const char* tc_sql =
                "INSERT INTO tool_calls (message_id, call_index, tool_call_id, name, arguments) "
                "VALUES (?, ?, ?, ?, ?)";
            rc = sqlite3_prepare_v2(db_, tc_sql, -1, &tc_stmt, nullptr);
            if (rc != SQLITE_OK) {
                sqlite3_finalize(tc_stmt);
                return -1;
            }
            sqlite3_bind_int64(tc_stmt, 1, msg_id);
            sqlite3_bind_int64(tc_stmt, 2, static_cast<int64_t>(i));
            sqlite3_bind_text(tc_stmt, 3, tc.id.c_str(), static_cast<int>(tc.id.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(tc_stmt, 4, tc.name.c_str(), static_cast<int>(tc.name.size()), SQLITE_TRANSIENT);
            sqlite3_bind_text(tc_stmt, 5, tc.arguments.c_str(), static_cast<int>(tc.arguments.size()), SQLITE_TRANSIENT);
            rc = sqlite3_step(tc_stmt);
            sqlite3_finalize(tc_stmt);
            if (rc != SQLITE_DONE) {
                return -1;
            }
        }

        return msg_id;
    } else {
        // Content-bearing assistant message
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO messages (seq, role, content, reasoning_content, retention) "
            "VALUES (?, 'assistant', ?, ?, 'preserve')";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_int64(stmt, 1, seq);
        sqlite3_bind_text(stmt, 2, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, reasoning.c_str(), static_cast<int>(reasoning.size()), SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            return -1;
        }
        return sqlite3_last_insert_rowid(db_);
    }
}

int64_t SessionDB::add_tool(const std::string& tool_call_id, const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t seq = claim_seq();
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (seq, role, content, tool_call_id, retention) "
        "VALUES (?, 'tool', ?, ?, 'droppable')";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, seq);
    sqlite3_bind_text(stmt, 2, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tool_call_id.c_str(), static_cast<int>(tool_call_id.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return -1;
    }
    return sqlite3_last_insert_rowid(db_);
}

json SessionDB::build_openai_payload(const std::string& system_prompt) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();

    // System prompt first
    arr.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});

    // Fetch all messages ordered by seq
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, role, content, reasoning_content, tool_call_id FROM messages ORDER BY seq";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return arr; // empty payload
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        const char* role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* reasoning = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* tool_call_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        json j;
        j["role"] = role;

        if (role && strcmp(role, "assistant") == 0) {
            // Check if this assistant message has tool_calls
            sqlite3_stmt* tc_stmt = nullptr;
            const char* tc_sql =
                "SELECT call_index, tool_call_id, name, arguments FROM tool_calls WHERE message_id = ? ORDER BY call_index";
            rc = sqlite3_prepare_v2(db_, tc_sql, -1, &tc_stmt, nullptr);
            if (rc == SQLITE_OK) {
                sqlite3_bind_int64(tc_stmt, 1, id);
                bool has_tc = false;
                json tc_arr = json::array();
                while (sqlite3_step(tc_stmt) == SQLITE_ROW) {
                    has_tc = true;
                    const char* tc_id = reinterpret_cast<const char*>(sqlite3_column_text(tc_stmt, 1));
                    const char* tc_name = reinterpret_cast<const char*>(sqlite3_column_text(tc_stmt, 2));
                    const char* tc_args = reinterpret_cast<const char*>(sqlite3_column_text(tc_stmt, 3));
                    tc_arr.push_back({{"id", sanitize_utf8(tc_id ? tc_id : "")},
                        {"type", "function"},
                        {"function",
                            {{"name", sanitize_utf8(tc_name ? tc_name : "")},
                                {"arguments", sanitize_utf8(tc_args ? tc_args : "")}}}});
                }
                sqlite3_finalize(tc_stmt);

                if (has_tc) {
                    j["content"] = nullptr;
                    j["tool_calls"] = std::move(tc_arr);
                    if (reasoning) {
                        j["reasoning_content"] = sanitize_utf8(reasoning);
                    }
                    arr.push_back(std::move(j));
                    continue;
                }
            }

            // No tool_calls: regular content-bearing message
            j["content"] = content ? sanitize_utf8(content) : "";
            if (reasoning && reasoning[0]) {
                j["reasoning_content"] = sanitize_utf8(reasoning);
            }
        } else {
            // user or tool
            j["content"] = content ? sanitize_utf8(content) : "";
        }

        if (tool_call_id && tool_call_id[0]) {
            j["tool_call_id"] = sanitize_utf8(tool_call_id);
        }

        arr.push_back(std::move(j));
    }

    sqlite3_finalize(stmt);
    return arr;
}

size_t SessionDB::estimate_total_tokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT content, reasoning_content, tool_call_id FROM messages ORDER BY seq";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return total;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* reasoning = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* tool_call_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        if (content) total += estimate_tokens(content);
        if (reasoning) total += estimate_tokens(reasoning);
        if (tool_call_id) total += estimate_tokens(tool_call_id);
        total += 20; // framing overhead
    }
    sqlite3_finalize(stmt);

    // Add tool call tokens
    sqlite3_stmt* tc_stmt = nullptr;
    const char* tc_sql = "SELECT name, arguments FROM tool_calls";
    rc = sqlite3_prepare_v2(db_, tc_sql, -1, &tc_stmt, nullptr);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(tc_stmt) == SQLITE_ROW) {
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(tc_stmt, 0));
            const char* args = reinterpret_cast<const char*>(sqlite3_column_text(tc_stmt, 1));
            if (name) total += estimate_tokens(name);
            if (args) total += estimate_tokens(args);
        }
        sqlite3_finalize(tc_stmt);
    }

    return total;
}

size_t SessionDB::estimate_droppable_tokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT content, tool_call_id FROM messages WHERE retention = 'droppable'";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return total;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* tool_call_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        if (content) total += estimate_tokens(content);
        if (tool_call_id) total += estimate_tokens(tool_call_id);
        total += 20; // framing overhead
    }
    sqlite3_finalize(stmt);

    return total;
}

void SessionDB::refresh_metadata(const std::string& model,
    int context_limit,
    const Usage& last_usage,
    int max_iterations,
    int tool_calls_used,
    int continuation_steps_used,
    int continuation_max_steps) {
    // Compute estimates outside the upsert lock — each method locks internally.
    size_t estimated_tok = estimate_total_tokens();
    size_t droppable_tok = estimate_droppable_tokens();
    size_t msg_count = message_count();

    std::lock_guard<std::mutex> lock(mutex_);

    // Ensure the metadata table exists
    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS metadata ("
        "key TEXT PRIMARY KEY,"
        "value TEXT"
        ")",
        nullptr, nullptr, nullptr);

    // Prepare upsert statement
    sqlite3_stmt* stmt = nullptr;
    const char* upsert_sql =
        "INSERT OR REPLACE INTO metadata (key, value) VALUES (?, ?)";
    if (sqlite3_prepare_v2(db_, upsert_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return;
    }

    auto upsert = [&](const std::string& key, const std::string& value) {
        sqlite3_bind_text(stmt, 1, key.c_str(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    };

    upsert("model", model);
    upsert("context_limit", std::to_string(context_limit));
    upsert("estimated_tokens", std::to_string(estimated_tok));
    upsert("droppable_tokens", std::to_string(droppable_tok));
    upsert("message_count", std::to_string(msg_count));

    // Usage from last API response
    upsert("last_prompt_tokens", std::to_string(last_usage.prompt_tokens));
    upsert("last_completion_tokens", std::to_string(last_usage.completion_tokens));
    upsert("last_total_tokens", std::to_string(last_usage.total_tokens));

    // Budgets
    upsert("max_tool_iterations", std::to_string(max_iterations));
    upsert("tool_calls_used", std::to_string(tool_calls_used));
    upsert("max_continuation_steps", std::to_string(continuation_max_steps));
    upsert("continuation_steps_used", std::to_string(continuation_steps_used));

    // Computed percentage (integer, e.g. "35" for 35%)
    if (context_limit > 0) {
        int pct = static_cast<int>(estimated_tok * 100 / context_limit);
        upsert("usage_percent", std::to_string(pct));
    } else {
        upsert("usage_percent", "0");
    }

    sqlite3_finalize(stmt);
}

void SessionDB::clear_conversation() {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_exec(db_, "DELETE FROM tool_calls", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "DELETE FROM messages", nullptr, nullptr, nullptr);
    next_seq_ = 0;
}

size_t SessionDB::message_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM messages", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return 0;
    }
    rc = sqlite3_step(stmt);
    size_t count = 0;
    if (rc == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

void SessionDB::truncate_conversation(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get the id of the Nth message (1-indexed, ordered by seq)
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id FROM messages ORDER BY seq LIMIT 1 OFFSET ?";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<int64_t>(n));

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t cutoff_id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        // Delete tool_calls for messages we're going to delete
        sqlite3_stmt* del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_,
            "DELETE FROM tool_calls WHERE message_id IN (SELECT id FROM messages WHERE id >= ?)",
            -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(del_stmt, 1, cutoff_id);
            sqlite3_step(del_stmt);
        }
        sqlite3_finalize(del_stmt);

        // Delete messages from cutoff onward
        del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE id >= ?", -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(del_stmt, 1, cutoff_id);
            sqlite3_step(del_stmt);
        }
        sqlite3_finalize(del_stmt);

        // Reset seq counter based on remaining max seq
        sqlite3_stmt* max_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(seq), 0) FROM messages", -1, &max_stmt, nullptr);
        if (rc == SQLITE_OK && sqlite3_step(max_stmt) == SQLITE_ROW) {
            next_seq_ = sqlite3_column_int64(max_stmt, 0);
        }
        sqlite3_finalize(max_stmt);
    } else {
        sqlite3_finalize(stmt);
    }
}

void SessionDB::prune_droppable() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Collect all tool_call_ids that still have a corresponding tool result
    std::unordered_set<std::string> active_tool_ids;
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT DISTINCT tool_call_id FROM messages WHERE role = 'tool' AND tool_call_id != ''";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (id) active_tool_ids.insert(id);
            }
        }
        sqlite3_finalize(stmt);
    }

    // Delete messages marked 'droppable'
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "DELETE FROM messages WHERE retention = 'droppable'";
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (stmt) {
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Delete orphaned assistant tool-call messages (no tool results for their calls)
    {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"(
            DELETE FROM messages WHERE id IN (
                SELECT m.id FROM messages m
                WHERE m.role = 'assistant'
                AND m.content IS NULL
                AND NOT EXISTS (
                    SELECT 1 FROM tool_calls tc
                    WHERE tc.message_id = m.id
                    AND tc.tool_call_id IN (SELECT tool_call_id FROM messages WHERE role = 'tool')
                )
            )
        )";
        sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (stmt) {
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    // Reset seq counter
    {
        sqlite3_stmt* max_stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_, "SELECT COALESCE(MAX(seq), 0) FROM messages", -1, &max_stmt, nullptr);
        if (rc == SQLITE_OK && sqlite3_step(max_stmt) == SQLITE_ROW) {
            next_seq_ = sqlite3_column_int64(max_stmt, 0);
        }
        sqlite3_finalize(max_stmt);
    }
}

Result<std::string> SessionDB::execute(const std::string& sql) {
    if (!db_) {
        return std::unexpected(std::string("SQLite database handle is null"));
    }

    // Reject excessively long queries (sanity limit)
    const size_t max_sql_len = 100 * 1024; // 100 KB
    if (sql.size() > max_sql_len) {
        return std::unexpected("SQL query too large (" + std::to_string(sql.size()) +
            " bytes, max " + std::to_string(max_sql_len) + ")");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_stmt* stmt = nullptr;
    const char* tail = nullptr;

    int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt, &tail);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(db_);
        return std::unexpected("SQL error: " + err);
    }

    // If no statement was produced (empty / whitespace-only input), treat as OK
    if (!stmt) {
        return std::string(R"({"ok": true})");
    }

    // Determine if this is a query (produces rows) or a DML/DDL statement
    int col_count = sqlite3_column_count(stmt);

    if (col_count > 0) {
        // ── SELECT / query-like statement ──
        json rows = json::array();

        while (true) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) {
                break;
            }
            if (rc == SQLITE_ROW) {
                json row;
                for (int i = 0; i < col_count; i++) {
                    const char* col_name = sqlite3_column_name(stmt, i);
                    std::string key = col_name ? col_name : "col" + std::to_string(i);

                    int col_type = sqlite3_column_type(stmt, i);
                    switch (col_type) {
                    case SQLITE_NULL:
                        row[key] = nullptr;
                        break;
                    case SQLITE_INTEGER:
                        row[key] = sqlite3_column_int64(stmt, i);
                        break;
                    case SQLITE_FLOAT:
                        row[key] = sqlite3_column_double(stmt, i);
                        break;
                    case SQLITE_TEXT: {
                        const char* text =
                            reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                        int len = sqlite3_column_bytes(stmt, i);
                        row[key] = std::string(text, static_cast<size_t>(len));
                        break;
                    }
                    case SQLITE_BLOB: {
                        // Represent BLOB as a hex string
                        const void* blob = sqlite3_column_blob(stmt, i);
                        int len = sqlite3_column_bytes(stmt, i);
                        std::ostringstream hex;
                        hex << "\\x";
                        for (int j = 0; j < len; j++) {
                            hex << std::hex << (static_cast<const unsigned char*>(blob)[j] >> 4)
                                << (static_cast<const unsigned char*>(blob)[j] & 0x0F);
                        }
                        row[key] = hex.str();
                        break;
                    }
                    default:
                        row[key] = nullptr;
                        break;
                    }
                }
                rows.push_back(std::move(row));
            } else {
                // SQLITE_BUSY, SQLITE_ERROR, etc.
                std::string err = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                return std::unexpected("SQL error during step: " + err);
            }
        }

        sqlite3_finalize(stmt);
        return rows.dump();
    } else {
        // ── DML / DDL (INSERT, UPDATE, DELETE, CREATE TABLE, etc.) ──
        rc = sqlite3_step(stmt);
        int finalized = sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            std::string err = sqlite3_errmsg(db_);
            return std::unexpected("SQL error: " + err);
        }

        if (finalized != SQLITE_OK) {
            std::string err = sqlite3_errmsg(db_);
            return std::unexpected("SQL finalize error: " + err);
        }

        int changes = sqlite3_changes(db_);
        json result = {{"rows_affected", changes}};
        return result.dump();
    }
}
