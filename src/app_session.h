#pragma once

#include "session_data.h"

#include <filesystem>
#include <string>

/// Manages a single app session persisted to ~/.local/state/cima/<session>.json.
///
/// The session is a single JSON file containing the entire state:
/// conversation, chat log, plan, provider, model, workspace, etc.
/// (version 2 format).
///
class AppSession {
  public:
    /// Construct/resume or create a new session.
    /// @param name   User-chosen session name (used as <session>.json filename)
    /// @param force  If true, integrity check failures produce warnings instead of errors
    AppSession(const std::string& name, bool force = false);

    AppSession(const AppSession&) = delete;
    AppSession& operator=(const AppSession&) = delete;
    AppSession(AppSession&&) = delete;
    AppSession& operator=(AppSession&&) = delete;

    // ── Path accessors ──

    /// Base directory for all sessions (~/.local/state/cima).
    static std::filesystem::path sessions_base_dir();

    /// Return the base directory (~/.local/state/cima).
    std::filesystem::path base_dir() const { return sessions_base_dir(); }

    /// Return the full path to the single session JSON file.
    std::string session_file_path() const;

    // ── Session data persistence ──

    /// Save session data atomically to the session file.
    /// Writes to a .tmp file, then renames atomically on POSIX.
    Result<void> save_session(const SessionData& data);

    /// Load session data from the session file.
    /// Returns a default-constructed SessionData if the file doesn't exist
    /// (first-run behaviour).  On parse error returns an error result.
    Result<SessionData> load_session();

    // ── Metadata ──

    /// The last working directory (loaded from session data).
    const std::string& last_cwd() const { return last_cwd_; }

    /// True if this session was freshly created (did not exist before).
    bool is_new_session() const { return is_new_; }

    /// The session name.
    const std::string& session_name() const { return session_name_; }

    /// Print a human-friendly "resuming session" or "starting new session" message.
    void print_welcome() const;

  private:
    std::string session_name_;
    std::string last_cwd_;
    bool is_new_ = false;
};
