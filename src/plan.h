#pragma once

#include "tools.h"
#include <string>
#include <vector>

// PlanBoard — per-session plan document storage.
// Holds a single plan markdown body plus an append-only list of comments.
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

  private:
    std::string plan_;
    std::vector<std::string> comments_;
};

// Tool factory declarations — each takes a PlanBoard reference to operate on.
Tool make_write_plan_tool(PlanBoard& board);
Tool make_read_plan_tool(PlanBoard& board);
Tool make_comment_plan_tool(PlanBoard& board);
