#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// Session folder contents as enumerated from the filesystem.
struct SessionFolderContents {
    std::vector<std::string> filenames; // all files in the session dir (excluding state.json)
};

/// Manages a single app session persisted to ~/.local/state/cima/<name>/.
///
/// Each session folder contains:
///   state.json   — manifest (version, last_cwd, file list)
///   wiki.db      — Wiki tab SQLite database
///   <name>.db    — One or more per-agent chat session databases
///
class AppSession {
  public:
    /// Construct/resume or create a new session.
    /// @param name   User-chosen session name (directory name under ~/.local/state/cima/)
    /// @param force  If true, integrity check failures produce warnings instead of errors
    AppSession(const std::string& name, bool force = false);

    AppSession(const AppSession&) = delete;
    AppSession& operator=(const AppSession&) = delete;
    AppSession(AppSession&&) = delete;
    AppSession& operator=(AppSession&&) = delete;

    // ── Path accessors ──
    const std::filesystem::path& session_dir() const { return session_dir_; }
    std::string wiki_db_path() const;
    std::vector<std::string> agent_db_filenames() const;
    std::string agent_db_path(const std::string& filename) const;

    // ── Manifest data ──
    const std::string& last_cwd() const { return last_cwd_; }
    void set_last_cwd(const std::string& cwd);

    // ── Agent DB management ──
    /// Add a new agent DB filename to the manifest (e.g. "Gandalf.db").
    void add_agent_db(const std::string& filename);

    /// Remove an agent DB filename from the manifest.
    void remove_agent_db(const std::string& filename);

    /// Persist state.json to disk.
    void save_manifest();

    /// True if this session was freshly created (did not exist before).
    bool is_new_session() const { return is_new_; }

    /// Print a human-friendly "resuming session" or "starting new session" message.
    void print_welcome() const;

    /// Return the base directory for all sessions (~/.local/state/cima).
    static std::filesystem::path sessions_base_dir();

  private:
    /// Load an existing session from disk.
    void load_existing(bool force);

    /// Create a brand new session directory and manifest.
    void create_new();

    /// Check integrity: files listed in state.json must match actual folder.
    /// On mismatch: if !force, prints error and throws; if force, prints warning and corrects.
    /// @return true if any issues were found (missing or extra files)
    bool verify_integrity(bool force);

    /// Enumerate all files currently in the session directory (excluding state.json).
    SessionFolderContents scan_folder() const;

    std::string session_name_;
    std::filesystem::path session_dir_;
    std::string last_cwd_;
    std::vector<std::string> agent_dbs_; // filenames like "Gandalf.db"
    bool is_new_ = false;
};
