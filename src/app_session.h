#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// Manages a single app session persisted to ~/.local/state/cima/<name>/.
///
/// Each session folder contains:
///   state.json   — JSON array of assistant filenames (e.g. ["Fingolfin.json"])
///                   Optionally also contains top-level "last_cwd" string.
///   wiki/        — Folder of markdown files (one per wiki page)
///   <name>.json  — One JSON file per assistant tab (consolidated data)
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
    std::string wiki_dir_path() const;

    /// Return the full path for a filename within this session directory.
    std::string session_file_path(const std::string& filename) const;

    // ── Manifest data ──
    const std::string& last_cwd() const { return last_cwd_; }
    void set_last_cwd(const std::string& cwd);

    /// Return the list of assistant filenames from state.json.
    const std::vector<std::string>& assistant_files() const { return assistant_files_; }

    /// Add an assistant filename to the manifest and persist.
    void add_assistant_file(const std::string& filename);

    /// Remove an assistant filename from the manifest and persist.
    void remove_assistant_file(const std::string& filename);

    /// Replace the entire assistant file list and persist.
    void set_assistant_files(const std::vector<std::string>& files);

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

    std::string session_name_;
    std::filesystem::path session_dir_;
    std::string last_cwd_;
    std::vector<std::string> assistant_files_;
    bool is_new_ = false;
};
