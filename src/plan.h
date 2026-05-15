#pragma once

#include "tools.h"
#include <string>
#include <vector>

// PlanBoard — per-session plan document storage.
// Holds a single plan markdown body plus an append-only list of comments.
//
// Unlike the old design, PlanBoard no longer auto-saves to a file on every
// mutation.  Persistence is handled externally by the session management
// code, which calls to_json() / from_json() to consolidate data into a
// single per-assistant JSON file.
class PlanBoard {
  public:
    PlanBoard() = default;

    PlanBoard(const PlanBoard&) = delete;
    PlanBoard& operator=(const PlanBoard&) = delete;
    PlanBoard(PlanBoard&&) = delete;
    PlanBoard& operator=(PlanBoard&&) = delete;

    /// Overwrite the plan with new markdown content.
    Result<void> write_plan(const std::string& markdown);

    /// Return the current plan + comments formatted as a single markdown document.
    Result<std::string> read_plan() const;

    /// Append a comment to the plan.
    Result<void> comment_plan(const std::string& markdown);

    // ── Serialization (used by external persistence) ──

    json to_json() const;
    void from_json(const json& j);

    // ── File persistence (legacy, may be removed) ──

    /// Set the file path for auto-save.  Load is separate (see load_from_file).
    void set_plan_file_path(const std::string& path) { plan_file_path_ = path; }

    /// Load plan + comments from a JSON file.  If the file does not exist
    /// or is corrupt, the plan is left empty (no error — first-run behaviour).
    Result<void> load_from_file(const std::string& path);

    /// Explicitly persist to the configured path (no-op if path is empty).
    /// Called automatically on every mutation.
    Result<void> save();

  private:
    std::string plan_;
    std::vector<std::string> comments_;
    std::string plan_file_path_;
};

// Tool factory declarations — each takes a PlanBoard reference to operate on.
Tool make_write_plan_tool(PlanBoard& board);
Tool make_read_plan_tool(PlanBoard& board);
Tool make_comment_plan_tool(PlanBoard& board);
