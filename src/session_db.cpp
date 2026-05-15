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
}

SessionDB::~SessionDB() {
    if (db_ && !auto_save_path_.empty()) {
        save_to_file(auto_save_path_);
    }
    if (db_) {
        sqlite3_close(db_);
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
    int rc = sqlite3_open(path.c_str(), &file_db);
    if (rc != SQLITE_OK) {
        // File may not exist yet — that's fine for new sessions
        if (rc == SQLITE_CANTOPEN) {
            sqlite3_close(file_db);
            return {};
        }
        std::string err = sqlite3_errmsg(file_db);
        sqlite3_close(file_db);
        return std::unexpected("Failed to open restore file: " + err);
    }
    auto file_cleanup =
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)>(file_db, sqlite3_close);

    sqlite3_backup* backup = sqlite3_backup_init(db_, "main", file_db, "main");
    if (!backup) {
        std::string err = sqlite3_errmsg(db_);
        return std::unexpected("Failed to init restore backup: " + err);
    }
    auto backup_cleanup =
        std::unique_ptr<sqlite3_backup, decltype(&sqlite3_backup_finish)>(
            backup, sqlite3_backup_finish);

    int rc2 = sqlite3_backup_step(backup, -1);
    if (rc2 != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(db_);
        return std::unexpected("Restore backup step failed: " + err);
    }

    return {};
}
