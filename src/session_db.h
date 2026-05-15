#pragma once

#include "config.h"
#include "types.h"

#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

/// Per-session in-memory SQLite database.
/// Each ChatSession owns one SessionDB instance. Agents can create tables,
/// insert data, and query results across tool calls within the same session.
///
/// This is pure scratch space — no conversation tables are created.
/// Conversation history is managed by the Conversation class instead.
class SessionDB {
  public:
    SessionDB();
    ~SessionDB();

    SessionDB(const SessionDB&) = delete;
    SessionDB& operator=(const SessionDB&) = delete;
    SessionDB(SessionDB&&) = delete;
    SessionDB& operator=(SessionDB&&) = delete;

    /// Execute an arbitrary SQL statement (DDL, DML, queries).
    ///   - For SELECT: returns JSON array of row objects, e.g.
    ///     `[{"col": "val"}, {"col": "val2"}]`
    ///   - For INSERT/UPDATE/DELETE: returns JSON object with rows_affected, e.g.
    ///     `{"rows_affected": 1}`
    ///   - For DDL (CREATE TABLE, etc.): returns `{"ok": true}`
    Result<std::string> execute(const std::string& sql);

    /// Direct access to the raw handle (for potential future extensions).
    sqlite3* handle() { return db_; }

    // ── Persistence ────────────────────────────────────────────────────

    /// Save the in-memory database to a file on disk (full backup).
    /// Returns an error if the backup fails.
    Result<void> save_to_file(const std::string& path);

    /// Load the database from a file on disk, replacing the current
    /// in-memory contents.  Returns an error if the file can't be opened
    /// or the backup fails.
    Result<void> load_from_file(const std::string& path);

    /// If set, the DB is automatically saved to this path on destruction
    /// and whenever the conversation changes significantly.
    void set_auto_save_path(const std::string& path) { auto_save_path_ = path; }
    const std::string& auto_save_path() const { return auto_save_path_; }

  private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_; // guard execute() — may be called from parallel tool paths

    // Optional auto-save path (set by ChatSession if persistence is configured)
    std::string auto_save_path_;
};
