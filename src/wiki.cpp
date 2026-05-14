#include "wiki.h"

#include <sqlite3.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

Wiki::Wiki(const std::string& db_path) : db_path_(db_path) {
    // Ensure parent directory exists
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(db_path).parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
    }

    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Wiki: failed to open database at " << db_path
                  << ": " << sqlite3_errmsg(db_) << std::endl;
        sqlite3_close(db_);
        db_ = nullptr;
        return;
    }

    init_tables();
}

Wiki::~Wiki() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

void Wiki::init_tables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS wiki_pages (
            title      TEXT PRIMARY KEY,
            body       TEXT NOT NULL DEFAULT '',
            updated_at TEXT NOT NULL DEFAULT (datetime('now'))
        )
    )";
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "Wiki: failed to create table: " << (err ? err : "unknown") << std::endl;
        sqlite3_free(err);
    }
}

// ── Helper: execute a query and return rows as JSON (for SELECT) ──
// Returns nullopt if no rows, or error string.
namespace {

struct QueryResult {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> columns;
};

std::optional<std::string> exec_query(sqlite3* db, const std::string& sql, QueryResult& out) {
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::string(sqlite3_errmsg(db));
    }

    // Get column count and names
    int col_count = sqlite3_column_count(stmt);
    out.columns.clear();
    for (int i = 0; i < col_count; i++) {
        out.columns.push_back(sqlite3_column_name(stmt, i));
    }

    out.rows.clear();
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::string> row;
        row.reserve(col_count);
        for (int i = 0; i < col_count; i++) {
            const char* text = (const char*)sqlite3_column_text(stmt, i);
            row.push_back(text ? text : "");
        }
        out.rows.push_back(std::move(row));
    }

    if (rc != SQLITE_DONE) {
        std::string err = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return err;
    }

    sqlite3_finalize(stmt);
    return std::nullopt; // no error
}

} // anonymous namespace

Result<std::vector<std::string>> Wiki::list_pages() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::unexpected("wiki database not available");
    }

    QueryResult qr;
    auto err = exec_query(db_, "SELECT title FROM wiki_pages ORDER BY title COLLATE NOCASE", qr);
    if (err) {
        return std::unexpected("wiki: " + *err);
    }

    std::vector<std::string> titles;
    titles.reserve(qr.rows.size());
    for (const auto& row : qr.rows) {
        if (!row.empty()) {
            titles.push_back(row[0]);
        }
    }
    return titles;
}

Result<std::string> Wiki::read_page(const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::unexpected("wiki database not available");
    }

    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT body FROM wiki_pages WHERE title = ?";
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), (int)title.size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char* body = (const char*)sqlite3_column_text(stmt, 0);
        std::string result = body ? body : "";
        sqlite3_finalize(stmt);
        return result;
    }

    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        return std::unexpected("no such page: " + title);
    }

    return std::unexpected(std::string(sqlite3_errmsg(db_)));
}

Result<void> Wiki::write_page(const std::string& title, const std::string& body) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::unexpected("wiki database not available");
    }

    sqlite3_stmt* stmt = nullptr;
    std::string sql = R"(
        INSERT INTO wiki_pages (title, body, updated_at)
        VALUES (?, ?, datetime('now'))
        ON CONFLICT(title) DO UPDATE SET
            body = excluded.body,
            updated_at = datetime('now')
    )";
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), (int)title.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, body.c_str(), (int)body.size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    return Result<void>();
}

Result<void> Wiki::edit_page(const std::string& title,
    const std::string& search,
    const std::string& replace) {
    if (search.empty()) {
        return std::unexpected("search string is required");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::unexpected("wiki database not available");
    }

    // Read current body
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "SELECT body FROM wiki_pages WHERE title = ?";
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), (int)title.size(), SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return std::unexpected("no such page: " + title);
        }
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    const char* body_cstr = (const char*)sqlite3_column_text(stmt, 0);
    std::string body = body_cstr ? body_cstr : "";
    sqlite3_finalize(stmt);

    // Count occurrences of search string
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find(search, pos)) != std::string::npos) {
        count++;
        pos += search.size();
    }

    if (count == 0) {
        return std::unexpected("Search string not found in page (0 matches). "
                               "Use read_wiki_page to verify the page contents.");
    }
    if (count > 1) {
        return std::unexpected("Search string found " + std::to_string(count) +
                               " times in page (expected exactly 1). "
                               "Include more surrounding context in the search string.");
    }

    // Find the unique occurrence
    pos = body.find(search);
    body.replace(pos, search.size(), replace);

    // Write back
    stmt = nullptr;
    sql = "UPDATE wiki_pages SET body = ?, updated_at = datetime('now') WHERE title = ?";
    rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, body.c_str(), (int)body.size(), SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, title.c_str(), (int)title.size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    return Result<void>();
}

Result<void> Wiki::delete_page(const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!db_) {
        return std::unexpected("wiki database not available");
    }

    sqlite3_stmt* stmt = nullptr;
    std::string sql = "DELETE FROM wiki_pages WHERE title = ?";
    int rc = sqlite3_prepare_v2(db_, sql.c_str(), (int)sql.size(), &stmt, nullptr);
    if (rc != SQLITE_OK) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    sqlite3_bind_text(stmt, 1, title.c_str(), (int)title.size(), SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return std::unexpected(std::string(sqlite3_errmsg(db_)));
    }

    int changes = sqlite3_changes(db_);
    if (changes == 0) {
        return std::unexpected("no such page: " + title);
    }

    return Result<void>();
}
