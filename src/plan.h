#pragma once

#include "tools.h"
#include <mutex>
#include <string>
#include <vector>

// PlanBoard — global singleton, thread-safe, in-memory plan document storage.
// Holds a single plan markdown body plus an append-only list of comments.
class PlanBoard {
  public:
    static PlanBoard& instance();

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
    PlanBoard() = default;
    mutable std::mutex mutex_;
    std::string plan_;
    std::vector<std::string> comments_;
};

// Tool factory declarations
Tool make_write_plan_tool();
Tool make_read_plan_tool();
Tool make_comment_plan_tool();
