#pragma once

#include "tools.h"
#include <string>

// PlanBoard — per-session plan document storage.
// Holds a single plan markdown body.
//
// Persistence is handled externally by the session management
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

    /// Return the current plan formatted as a markdown document.
    Result<std::string> read_plan() const;

    // ── Serialization (used by external persistence) ──

    json to_json() const;
    void from_json(const json& j);

  private:
    std::string plan_;
};

extern PlanBoard plan;

// Tool factory declarations — each takes a PlanBoard reference to operate on.
Tool make_write_plan_tool(PlanBoard& board);
Tool make_read_plan_tool(PlanBoard& board);
