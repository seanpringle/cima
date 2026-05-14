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
/// The conversation history is stored in two tables:
///   messages  — one row per message (user, assistant, tool)
///   tool_calls — one row per tool_call within an assistant message
/// Agents can read/write these tables directly via the query_session tool
/// to manage their own context (summarize, prune, reorganize).
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

    // ── Conversation management ──────────────────────────────────────────
    // These methods are used by ChatSession to persist the conversation.
    // The same data is also accessible via SQL through query_session.

    /// Initialize conversation tables (messages, tool_calls).
    /// Safe to call multiple times (CREATE TABLE IF NOT EXISTS).
    void init_conversation_tables();

    /// Add a user message. Returns the new message id.
    int64_t add_user(const std::string& content);

    /// Add a system message (e.g. usage notices). Returns the new message id.
    int64_t add_system(const std::string& content,
        const std::string& retention = "droppable");

    /// Add an assistant message. If tool_calls is non-empty, the message
    /// content is set to NULL and tool_calls are inserted into the
    /// tool_calls table.  Returns the new message id.
    int64_t add_assistant(const std::string& content,
        const std::string& reasoning = {},
        const std::vector<ToolCall>& tool_calls = {});

    /// Add a tool result message. Returns the new message id.
    int64_t add_tool(const std::string& tool_call_id, const std::string& content);

    /// Build the OpenAI-compatible messages array from the DB contents.
    /// Prepends the system prompt as the first message.
    json build_openai_payload(const std::string& system_prompt) const;

    /// Estimate total tokens for all messages currently in the DB.
    size_t estimate_total_tokens() const;

    /// Return the number of messages in the conversation.
    size_t message_count() const;

    /// Truncate conversation to at most N messages (for rollback on error).
    void truncate_conversation(size_t n);

    /// Estimate tokens for messages with retention='droppable'.
    size_t estimate_droppable_tokens() const;

    /// Populate/refresh the metadata table with current session state.
    /// Creates the table if it doesn't exist.
    void refresh_metadata(const std::string& model,
        int context_limit,
        const Usage& last_usage,
        int max_iterations,
        int tool_calls_used,
        int continuation_steps_used,
        int continuation_max_steps,
        const std::string& assistant_name = {});

    /// Delete all messages tagged 'droppable' and clean up orphaned
    /// assistant tool-call messages (same logic as old Conversation::compact).
    void prune_droppable();



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

    // Next sequence number for messages
    int64_t next_seq_ = 0;

    // Optional auto-save path (set by ChatSession if persistence is configured)
    std::string auto_save_path_;

    // Internal helper: get the next seq and increment
    int64_t claim_seq();
};
