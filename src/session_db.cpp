#include "session_db.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <sstream>
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
    if (db_) {
        sqlite3_close(db_);
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
