#include "jobs.h"

#include <algorithm>
#include <sstream>

// ===================================================================
// JobBoard singleton
// ===================================================================

JobBoard& JobBoard::instance() {
    static JobBoard board;
    return board;
}

// ===================================================================
// JobBoard operations
// ===================================================================

Result<void> JobBoard::open_job(const std::string& name, const std::string& description) {
    if (name.empty()) {
        return std::unexpected(std::string("job name must not be empty"));
    }
    if (description.empty()) {
        return std::unexpected(std::string("job description must not be empty"));
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (jobs_.contains(name)) {
        return std::unexpected("a job named \"" + name + "\" already exists");
    }

    jobs_[name] = Job{name, description, {}};
    return {};
}

Result<std::vector<std::string>> JobBoard::list_jobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(jobs_.size());
    for (const auto& [name, _] : jobs_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Result<Job> JobBoard::read_job(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) {
        return std::unexpected("job not found: \"" + name + "\"");
    }
    return it->second;
}

Result<void> JobBoard::comment_job(const std::string& name, const std::string& comment) {
    if (comment.empty()) {
        return std::unexpected("comment must not be empty");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) {
        return std::unexpected("job not found: \"" + name + "\"");
    }
    it->second.comments.push_back(comment);
    return {};
}

Result<void> JobBoard::close_job(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) {
        return std::unexpected("job not found: \"" + name + "\"");
    }
    jobs_.erase(it);
    return {};
}

Result<void> JobBoard::edit_job(const std::string& name,
    const std::string& new_name,
    const std::string& new_description) {
    if (new_name.empty() && new_description.empty()) {
        return std::unexpected(
            "at least one of new_name or new_description must be provided");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_.find(name);
    if (it == jobs_.end()) {
        return std::unexpected("job not found: \"" + name + "\"");
    }

    Job job = it->second; // copy

    if (!new_name.empty()) {
        if (new_name != name && jobs_.contains(new_name)) {
            return std::unexpected(
                "a job named \"" + new_name + "\" already exists");
        }
        job.name = new_name;
    }

    if (!new_description.empty()) {
        job.description = new_description;
    }

    jobs_.erase(it);          // erase old key
    jobs_[job.name] = job;    // insert with (possibly new) key

    return {};
}

// ===================================================================
// Tool: open_job
// ===================================================================

Tool make_open_job_tool() {
    Tool t;
    t.name = "open_job";
    t.description = "Create a job ticket with an informative name and a detailed description, "
                    "suitable for another agent to pick up.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description",
                        "Name of the job (must be unique)"}}},
                {"description",
                    {{"type", "string"},
                        {"description",
                            "Detailed markdown description of the job"}}}}},
        {"required", {"name", "description"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto description = args.value("description", std::string());
        auto result = JobBoard::instance().open_job(name, description);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Job opened: \"" + name + "\"";
    };
    return t;
}

// ===================================================================
// Tool: list_jobs
// ===================================================================

Tool make_list_jobs_tool() {
    Tool t;
    t.name = "list_jobs";
    t.description = "Return a list of open job names.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [](const json& /*args*/) -> Result<std::string> {
        auto names = JobBoard::instance().list_jobs();
        if (!names) {
            return std::unexpected(names.error());
        }
        json arr = json::array();
        for (const auto& n : *names) {
            arr.push_back(n);
        }
        return arr.dump();
    };
    return t;
}

// ===================================================================
// Tool: read_job
// ===================================================================

Tool make_read_job_tool() {
    Tool t;
    t.name = "read_job";
    t.description = "Retrieve an open job document (name + description + comments as markdown).";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description", "Name of the job to retrieve"}}}}},
        {"required", {"name"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto job = JobBoard::instance().read_job(name);
        if (!job) {
            return std::unexpected(job.error());
        }
        std::ostringstream ss;
        ss << "# " << job->name << "\n\n";
        ss << job->description << "\n\n";
        if (!job->comments.empty()) {
            ss << "## Comments\n\n";
            for (size_t i = 0; i < job->comments.size(); i++) {
                ss << "### Comment " << (i + 1) << "\n\n";
                ss << job->comments[i] << "\n\n";
            }
        }
        return ss.str();
    };
    return t;
}

// ===================================================================
// Tool: comment_job
// ===================================================================

Tool make_comment_job_tool() {
    Tool t;
    t.name = "comment_job";
    t.description = "Append a comment to a job. Detailed as you like.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description", "Name of the job to comment on"}}},
                {"comment",
                    {{"type", "string"},
                        {"description",
                            "Markdown comment to append to the job"}}}}},
        {"required", {"name", "comment"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto comment = args.value("comment", std::string());
        auto result = JobBoard::instance().comment_job(name, comment);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Comment added to job: \"" + name + "\"";
    };
    return t;
}

// ===================================================================
// Tool: close_job
// ===================================================================

Tool make_close_job_tool() {
    Tool t;
    t.name = "close_job";
    t.description = "Close a job. Removed from the list.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description", "Name of the job to close"}}}}},
        {"required", {"name"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto result = JobBoard::instance().close_job(name);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Job closed: \"" + name + "\"";
    };
    return t;
}

// ===================================================================
// Tool: edit_job
// ===================================================================

Tool make_edit_job_tool() {
    Tool t;
    t.name = "edit_job";
    t.description = "Edit the name and/or description of an existing job. "
                    "At least one of new_name or new_description must be provided.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description",
                        "Name of the existing job to edit"}}},
                {"new_name",
                    {{"type", "string"},
                        {"description",
                            "New name for the job (optional, must be unique if provided)"}}},
                {"new_description",
                    {{"type", "string"},
                        {"description",
                            "New description for the job (optional)"}}}}},
        {"required", {"name"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto new_name = args.value("new_name", std::string());
        auto new_description = args.value("new_description", std::string());
        auto result = JobBoard::instance().edit_job(name, new_name, new_description);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Job edited: \"" + (new_name.empty() ? name : new_name) + "\"";
    };
    return t;
}

// ===================================================================
// Convenience: register all job tools
// ===================================================================

void add_job_tools(ToolRegistry& registry) {
    registry.add(make_open_job_tool());
    registry.add(make_list_jobs_tool());
    registry.add(make_read_job_tool());
    registry.add(make_comment_job_tool());
    registry.add(make_close_job_tool());
    registry.add(make_edit_job_tool());
}
