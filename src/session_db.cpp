#include "session_db.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <string>
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
    if (db_ && !auto_save_path_.empty()) {
        save_to_file(auto_save_path_);
    }
    if (db_) {
        sqlite3_close(db_);
    }
}

void SessionDB::init_conversation_tables() {
    std::lock_guard<std::mutex> lock(mutex_);
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS messages (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            role TEXT NOT NULL,
            content TEXT,
            reasoning_content TEXT DEFAULT '',
            tool_data TEXT DEFAULT '',
            suggested_retention TEXT DEFAULT 'preserve'
        );
        CREATE TABLE IF NOT EXISTS metadata (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
    }
}

// ---------------------------------------------------------------------------
// Add message helpers
// ---------------------------------------------------------------------------

int64_t SessionDB::add_user(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (role, content, suggested_retention) VALUES ('user', ?, 'preserve')";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

int64_t SessionDB::add_notice(const std::string& content) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (role, content, suggested_retention) VALUES ('user', ?, 'droppable')";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

int64_t SessionDB::add_system(const std::string& content,
    const std::string& suggested_retention) {
    std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (role, content, suggested_retention) VALUES ('system', ?, ?)";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, suggested_retention.c_str(),
        static_cast<int>(suggested_retention.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

int64_t SessionDB::add_assistant(const std::string& content,
    const std::string& reasoning,
    const std::vector<ToolCall>& tool_calls) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!tool_calls.empty()) {
        // Serialize tool calls to JSON array
        json td = json::array();
        for (const auto& tc : tool_calls) {
            td.push_back({{"id", tc.id},
                {"type", "function"},
                {"function",
                    {{"name", tc.name}, {"arguments", tc.arguments}}},
                {"result", nullptr}});
        }
        std::string td_str = td.dump();

        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "INSERT INTO messages (role, content, reasoning_content, tool_data, suggested_retention) "
            "VALUES ('assistant', NULL, ?, ?, 'preserve')";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
        sqlite3_bind_text(stmt, 1, reasoning.c_str(), static_cast<int>(reasoning.size()), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, td_str.c_str(), static_cast<int>(td_str.size()), SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) return -1;
        return sqlite3_last_insert_rowid(db_);
    }

    // Content-bearing assistant message (no tool calls)
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO messages (role, content, reasoning_content, suggested_retention) "
        "VALUES ('assistant', ?, ?, 'preserve')";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, content.c_str(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, reasoning.c_str(), static_cast<int>(reasoning.size()), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db_);
}

void SessionDB::add_tool(int64_t message_id,
    const std::string& tool_call_id,
    const std::string& content) {
    auto sanitized = sanitize_utf8(content);

    std::lock_guard<std::mutex> lock(mutex_);

    // Read the current tool_data JSON
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT tool_data FROM messages WHERE id = ?";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return;
    }
    sqlite3_bind_int64(stmt, 1, message_id);
    rc = sqlite3_step(stmt);
    std::string td_str;
    if (rc == SQLITE_ROW) {
        const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (text) td_str = text;
    }
    sqlite3_finalize(stmt);

    if (td_str.empty()) return;

    // Parse, find the entry, set its result
    json td = json::parse(td_str);
    for (auto& entry : td) {
        if (entry["id"] == tool_call_id) {
            entry["result"] = sanitized;
            break;
        }
    }

    // Write back
    std::string updated = td.dump();
    sqlite3_stmt* up_stmt = nullptr;
    const char* up_sql = "UPDATE messages SET tool_data = ? WHERE id = ?";
    rc = sqlite3_prepare_v2(db_, up_sql, -1, &up_stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(up_stmt);
        return;
    }
    sqlite3_bind_text(up_stmt, 1, updated.c_str(), static_cast<int>(updated.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(up_stmt, 2, message_id);
    sqlite3_step(up_stmt);
    sqlite3_finalize(up_stmt);
}

// ---------------------------------------------------------------------------
// API payload builder
// ---------------------------------------------------------------------------

json SessionDB::build_openai_payload(const std::string& system_prompt) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();

    arr.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, role, content, reasoning_content, tool_data FROM messages ORDER BY id";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return arr;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* reasoning = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* tool_data_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));

        std::string role_s = role ? role : "";

        if (role_s == "assistant" && tool_data_raw && tool_data_raw[0]) {
            // Assistant with tool_data — expand into assistant + tool messages
            json td = json::parse(tool_data_raw);
            json j;
            j["role"] = "assistant";
            j["content"] = nullptr;
            if (reasoning && reasoning[0]) {
                j["reasoning_content"] = sanitize_utf8(reasoning);
            }

            json tc_arr = json::array();
            for (const auto& entry : td) {
                json tc;
                tc["id"] = entry["id"];
                tc["type"] = "function";
                tc["function"] = entry["function"];
                tc_arr.push_back(std::move(tc));
            }
            j["tool_calls"] = std::move(tc_arr);
            arr.push_back(std::move(j));

            // Emit tool result messages for entries with non-null results
            for (const auto& entry : td) {
                if (!entry.contains("result") || entry["result"].is_null()) continue;
                json tr;
                tr["role"] = "tool";
                tr["tool_call_id"] = entry["id"];
                tr["content"] = sanitize_utf8(entry["result"].get<std::string>());
                arr.push_back(std::move(tr));
            }
        } else {
            // user, system, or assistant without tool_data
            json j;
            j["role"] = role_s;
            j["content"] = content ? sanitize_utf8(content) : "";

            if (role_s == "assistant" && reasoning && reasoning[0]) {
                j["reasoning_content"] = sanitize_utf8(reasoning);
            }

            arr.push_back(std::move(j));
        }
    }

    sqlite3_finalize(stmt);
    return arr;
}

// ---------------------------------------------------------------------------
// Token estimation
// ---------------------------------------------------------------------------

size_t SessionDB::estimate_total_tokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT content, reasoning_content, tool_data FROM messages ORDER BY id";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return total;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* reasoning = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* tool_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));

        if (content) total += estimate_tokens(content);
        if (reasoning) total += estimate_tokens(reasoning);
        if (tool_data) total += estimate_tokens(tool_data);
        total += 20; // framing overhead
    }
    sqlite3_finalize(stmt);
    return total;
}

size_t SessionDB::estimate_droppable_tokens() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;

    // Count tool_data JSON content as compactable
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT tool_data FROM messages WHERE tool_data != ''";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return total;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char* tool_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (tool_data) total += estimate_tokens(tool_data);
    }
    sqlite3_finalize(stmt);

    return total;
}

// ---------------------------------------------------------------------------
// Metadata
// ---------------------------------------------------------------------------

void SessionDB::refresh_metadata(const std::string& model,
    int context_limit,
    const Usage& last_usage,
    int max_iterations,
    int tool_calls_used,
    int continuation_steps_used,
    int continuation_max_steps,
    const std::string& assistant_name) {
    size_t estimated_tok = estimate_total_tokens();
    size_t droppable_tok = estimate_droppable_tokens();
    size_t msg_count = message_count();

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3_exec(db_,
        "CREATE TABLE IF NOT EXISTS metadata ("
        "key TEXT PRIMARY KEY,"
        "value TEXT"
        ")",
        nullptr, nullptr, nullptr);

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
    upsert("assistant_name", assistant_name);
    upsert("context_limit", std::to_string(context_limit));
    upsert("estimated_tokens", std::to_string(estimated_tok));
    upsert("droppable_tokens", std::to_string(droppable_tok));
    upsert("message_count", std::to_string(msg_count));
    upsert("tool_calls_used", std::to_string(tool_calls_used));
    upsert("max_tool_iterations", std::to_string(max_iterations));
    upsert("continuation_steps_used", std::to_string(continuation_steps_used));
    upsert("continuation_max_steps", std::to_string(continuation_max_steps));

    // Computed percentage (integer, e.g. "35" for 35%)
    if (context_limit > 0) {
        int pct = static_cast<int>(estimated_tok * 100 / context_limit);
        upsert("context_usage_percent", std::to_string(pct));
    } else {
        upsert("context_usage_percent", "0");
    }

    if (last_usage.prompt_tokens > 0) {
        upsert("last_prompt_tokens", std::to_string(last_usage.prompt_tokens));
        upsert("last_completion_tokens", std::to_string(last_usage.completion_tokens));
        upsert("last_total_tokens", std::to_string(last_usage.total_tokens));
    }

    sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// Count / truncate
// ---------------------------------------------------------------------------

size_t SessionDB::message_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM messages";
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return count;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = static_cast<size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return count;
}

void SessionDB::truncate_conversation(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Find the id of the Nth message (0-indexed by id order)
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id FROM messages ORDER BY id LIMIT 1 OFFSET ?";
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

        // Delete everything from cutoff onward
        sqlite3_stmt* del_stmt = nullptr;
        rc = sqlite3_prepare_v2(db_, "DELETE FROM messages WHERE id >= ?", -1, &del_stmt, nullptr);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(del_stmt, 1, cutoff_id);
            sqlite3_step(del_stmt);
        }
        sqlite3_finalize(del_stmt);
    } else {
        sqlite3_finalize(stmt);
    }
}

// ---------------------------------------------------------------------------
// Raw SQL execution for the agent
// ---------------------------------------------------------------------------

Result<std::string> SessionDB::execute(const std::string& sql) {
    if (!db_) {
        return std::unexpected(std::string("SQLite database handle is null"));
    }

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

    if (!stmt) {
        return std::string(R"({"ok": true})");
    }

    int col_count = sqlite3_column_count(stmt);

    if (col_count > 0) {
        json rows = json::array();
        while (true) {
            rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
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
                        row[key] = sanitize_utf8(std::string(text, static_cast<size_t>(len)));
                        break;
                    }
                    case SQLITE_BLOB: {
                        const void* blob = sqlite3_column_blob(stmt, i);
                        int len = sqlite3_column_bytes(stmt, i);
                        std::ostringstream hex;
                        hex << "\\x";
                        for (int j = 0; j < len; j++) {
                            hex << std::hex
                                << (static_cast<const unsigned char*>(blob)[j] >> 4)
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
                std::string err = sqlite3_errmsg(db_);
                sqlite3_finalize(stmt);
                return std::unexpected("SQL error during step: " + err);
            }
        }
        sqlite3_finalize(stmt);
        return rows.dump();
    }

    // DML / DDL
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
    return json({{"rows_affected", changes}}).dump();
}

// ===================================================================
// Persistence — save / load via SQLite backup API
// ===================================================================

Result<void> SessionDB::save_to_file(const std::string& path) {
    if (!db_) {
        return std::unexpected(std::string("Database handle is null"));
    }

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3* file_db = nullptr;
    int rc = sqlite3_open(path.c_str(), &file_db);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(file_db);
        sqlite3_close(file_db);
        return std::unexpected("Failed to open save file: " + err);
    }
    auto file_cleanup =
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(file_db, sqlite3_close);

    sqlite3_backup* backup = sqlite3_backup_init(file_db, "main", db_, "main");
    if (!backup) {
        std::string err = sqlite3_errmsg(file_db);
        return std::unexpected("Failed to init backup: " + err);
    }
    auto backup_cleanup =
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)>(
            backup, sqlite3_backup_finish);

    int rc2 = sqlite3_backup_step(backup, -1);
    if (rc2 != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(file_db);
        return std::unexpected("Backup failed: " + err);
    }

    return {};
}

Result<void> SessionDB::load_from_file(const std::string& path) {
    if (!db_) {
        return std::unexpected(std::string("Database handle is null"));
    }

    std::lock_guard<std::mutex> lock(mutex_);

    sqlite3* file_db = nullptr;
    int rc = sqlite3_open_v2(path.c_str(), &file_db,
        SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(file_db);
        sqlite3_close(file_db);
        return std::unexpected("Failed to open load file: " + err);
    }
    auto file_cleanup =
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(file_db, sqlite3_close);

    sqlite3* new_db = nullptr;
    rc = sqlite3_open(":memory:", &new_db);
    if (rc != SQLITE_OK) {
        std::string err = sqlite3_errmsg(new_db);
        sqlite3_close(new_db);
        return std::unexpected("Failed to create in-memory DB: " + err);
    }

    sqlite3_backup* backup = sqlite3_backup_init(new_db, "main", file_db, "main");
    if (!backup) {
        std::string err = sqlite3_errmsg(new_db);
        sqlite3_close(new_db);
        return std::unexpected("Failed to init backup: " + err);
    }
    auto backup_cleanup =
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)>(
            backup, sqlite3_backup_finish);

    rc = sqlite3_backup_step(backup, -1);
    if (rc != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(new_db);
        sqlite3_close(new_db);
        return std::unexpected("Backup failed: " + err);
    }
    backup_cleanup.reset();
    file_cleanup.reset();

    sqlite3_close(db_);
    db_ = new_db;

    // Ensure tables exist
    {
        const char* tbl_sql = R"(
            CREATE TABLE IF NOT EXISTS messages (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                role TEXT NOT NULL,
                content TEXT,
                reasoning_content TEXT DEFAULT '',
                tool_data TEXT DEFAULT '',
                suggested_retention TEXT DEFAULT 'preserve'
            );
            CREATE TABLE IF NOT EXISTS metadata (
                key TEXT PRIMARY KEY,
                value TEXT
            );
        )";
        char* err = nullptr;
        sqlite3_exec(db_, tbl_sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);
    }

    return {};
}
