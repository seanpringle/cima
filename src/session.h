#pragma once

#include "session_data.h"
#include "plan.h"

#include <filesystem>
#include <string>

class ChatSession;

/// Manages a single session persisted to ~/.local/state/cima/<session>.json.
///
/// The session is a single JSON file containing the entire state:
/// conversation, chat log, plan, provider, model, workspace, etc.
/// (version 2 format).
///
class Session {
  public:
    /// Construct/resume or create a new session.
    /// @param name   User-chosen session name (used as <session>.json filename)
    Session(const std::string& name, ConfigPtr cfg, PlanBoardPtr plan);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

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
    Result<void> save_session();
    Result<void> load_session();

    SessionData& session_data() { return session_data_; }
    const SessionData& session_data() const { return session_data_; }

    /// Access the config.
    ConfigPtr config() const { return cfg_; }

    /// Access the plan board.
    PlanBoard& plan() { return *plan_; }
    const PlanBoard& plan() const { return *plan_; }

    // ── Metadata ──

    /// The last working directory (loaded from session data).
    const std::string& last_cwd() const { return last_cwd_; }

    /// True if this session was freshly created (did not exist before).
    bool is_new_session() const { return is_new_; }

    /// The session name.
    const std::string& session_name() const { return session_name_; }

    /// Print a human-friendly "resuming session" or "starting new session" message.
    void print_welcome() const;

    // ── Knob accessors (0 = use code default from Config) ──
    int max_tool_iterations() const;
    int subagent_timeout() const;
    int bash_timeout() const;
    int grep_timeout() const;
    int web_search_timeout() const;
    int web_fetch_timeout() const;

    void set_max_tool_iterations(int v);
    void set_subagent_timeout(int v);
    void set_bash_timeout(int v);
    void set_grep_timeout(int v);
    void set_web_search_timeout(int v);
    void set_web_fetch_timeout(int v);

    /// Apply knob overrides to a ChatSession (rebuild tools with custom timeouts).
    void apply_knobs_to(ChatSession& session) const;

  private:
    std::string session_name_;
    std::string last_cwd_;
    bool is_new_ = false;
    SessionData session_data_;
    ConfigPtr cfg_;
    PlanBoardPtr plan_;
};
