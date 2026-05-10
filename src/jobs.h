#pragma once

#include "tools.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// Job — a lightweight ticket with name, description, and append-only comments
// ---------------------------------------------------------------------------

struct Job {
    std::string name;
    std::string description;
    std::vector<std::string> comments;
};

// ---------------------------------------------------------------------------
// JobBoard — global singleton, thread-safe, in-memory job storage
// ---------------------------------------------------------------------------

class JobBoard {
  public:
    static JobBoard& instance();

    JobBoard(const JobBoard&) = delete;
    JobBoard& operator=(const JobBoard&) = delete;
    JobBoard(JobBoard&&) = delete;
    JobBoard& operator=(JobBoard&&) = delete;

    Result<void> open_job(const std::string& name, const std::string& description);
    Result<std::vector<std::string>> list_jobs() const;
    Result<Job> read_job(const std::string& name) const;
    Result<void> comment_job(const std::string& name, const std::string& comment);
    Result<void> close_job(const std::string& name);
    Result<void> edit_job(const std::string& name,
        const std::string& new_name,
        const std::string& new_description);

  private:
    JobBoard() = default;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_;
};

// ---------------------------------------------------------------------------
// Tool factory functions
// ---------------------------------------------------------------------------

Tool make_open_job_tool();
Tool make_list_jobs_tool();
Tool make_read_job_tool();
Tool make_comment_job_tool();
Tool make_close_job_tool();
Tool make_edit_job_tool();

/// Convenience: register all six job tools on a ToolRegistry.
void add_job_tools(ToolRegistry& registry);
