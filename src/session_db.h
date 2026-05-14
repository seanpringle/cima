#pragma once

#include "config.h"

#include <mutex>
#include <string>

struct sqlite3;

/// Per-session in-memory SQLite database.
/// Each ChatSession owns one SessionDB instance. Agents can create tables,
/// insert data, and query results across tool calls within the same session.
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

  private:
    sqlite3* db_ = nullptr;
    std::mutex mutex_; // guard execute() — may be called from parallel tool paths
};
