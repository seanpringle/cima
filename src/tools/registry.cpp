#include "tools.h"

#include <future>
#include <memory>
#include <thread>

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) {
    std::lock_guard<std::mutex> lock(mutex_);
    tools_.push_back(std::move(tool));
}

void ToolRegistry::add_defaults(const std::string& safe_dir,
    const Config& config,
    bool include_write,
    FileModifiedCallback on_file_modified,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    add_defaults(std::make_shared<std::string>(safe_dir), config, include_write, std::move(on_file_modified), std::move(tool_logs));
}

void ToolRegistry::add_defaults(std::shared_ptr<std::string> safe_dir_ptr,
    const Config& config,
    bool include_write,
    FileModifiedCallback on_file_modified,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    const auto& read_only_paths = config.read_only_paths;

    // ── Read-only tools (receive whitelist for extra path access) ──
    {
        auto t = make_list_directory_tool(safe_dir_ptr, read_only_paths, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_stat_file_tool(safe_dir_ptr, read_only_paths);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_read_file_tool(safe_dir_ptr, read_only_paths);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_read_file_lines_tool(safe_dir_ptr, read_only_paths);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_grep_files_tool(safe_dir_ptr, read_only_paths, config.grep_timeout, cancelled_, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_project_tree_tool(safe_dir_ptr, read_only_paths, config.project_tree_timeout, cancelled_, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_search_tool(config.web_search_timeout, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_fetch_tool(config.web_fetch_timeout, cancelled_, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_status_tool(safe_dir_ptr, config.git_status_timeout);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_diff_tool(safe_dir_ptr, config.git_diff_timeout, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_log_tool(safe_dir_ptr, config.git_log_timeout, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_show_tool(safe_dir_ptr, config.git_log_timeout, tool_logs);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }

    // ── Write tools ──
    if (include_write) {
        {
            auto t = make_write_file_tool(safe_dir_ptr, on_file_modified);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_edit_file_tool(safe_dir_ptr, on_file_modified);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_run_bash_tool(safe_dir_ptr, config.bash_timeout, cancelled_, tool_logs);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_git_add_tool(safe_dir_ptr, config.git_add_timeout);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_git_commit_tool(safe_dir_ptr, config.git_commit_timeout);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_git_restore_tool(safe_dir_ptr, config.git_status_timeout);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_delete_file_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_move_file_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_rename_file_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_create_directory_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_delete_directory_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
    }

}

json ToolRegistry::to_openai_tools() const {
    return to_openai_tools(nullptr);
}

json ToolRegistry::to_openai_tools(const std::set<std::string>* only_these) const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (const auto& t : tools_) {
        if (only_these && !only_these->count(t.name))
            continue;
        arr.push_back({{"type", "function"},
            {"function",
                {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    }
    return arr;
}

std::set<std::string> ToolRegistry::tool_names_by_permission(ToolPermission perm) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::set<std::string> names;
    for (const auto& t : tools_) {
        if (t.permission == perm)
            names.insert(t.name);
    }
    return names;
}

Result<std::string> ToolRegistry::execute(const std::string& name, const std::string& args_json) {
    json args;
    try {
        args = json::parse(args_json);
    } catch (const json::parse_error& e) {
        return std::unexpected("invalid JSON arguments: " + std::string(e.what()));
    }

    // Find tool and copy the execute function under the lock so that
    // concurrent add()/remove() cannot invalidate the pointer.
    std::string tool_name;
    int timeout_sec = 0;
    std::function<Result<std::string>(const json&)> exec_fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Tool* tool = find(name);
        if (!tool) {
            return std::unexpected("unknown tool: " + name);
        }
        tool_name = tool->name;
        timeout_sec = tool->timeout_sec;
        exec_fn = tool->execute;  // copy — keeps callable alive after lock release
    }

    if (timeout_sec > 0) {
        // Use packaged_task + detached thread so we can return on timeout
        // without blocking in the future destructor (std::async's future
        // destructor blocks until the task finishes, defeating the timeout).
        //
        // exec_fn was already copied under the lock above, so the callable
        // outlives the Tool even if the registry is destroyed while the
        // detached thread is running (use-after-free guard).
        auto task = std::make_shared<std::packaged_task<Result<std::string>()>>(
            [exec_fn = std::move(exec_fn), args = std::move(args)]() { return exec_fn(args); });
        auto future = task->get_future();
        std::thread t([task]() { (*task)(); });
        t.detach();

        auto status = future.wait_for(std::chrono::seconds(timeout_sec));
        if (status == std::future_status::timeout) {
            return std::unexpected("tool '" + tool_name + "' timed out after " +
                std::to_string(timeout_sec) + "s");
        }
        try {
            return future.get();
        } catch (const std::exception& e) {
            return std::unexpected(
                std::string("tool '") + tool_name + "' threw: " + e.what());
        }
    }

    try {
        return exec_fn(args);
    } catch (const std::exception& e) {
        return std::unexpected(
            std::string("tool '") + tool_name + "' threw: " + e.what());
    }
}

Tool* ToolRegistry::find(const std::string& name) {
    // NOTE: caller must hold mutex_ — this is a private helper
    for (auto& t : tools_) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}

bool ToolRegistry::has(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& t : tools_) {
        if (t.name == name)
            return true;
    }
    return false;
}

std::vector<std::string> ToolRegistry::tool_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& t : tools_) {
        names.push_back(t.name);
    }
    return names;
}

bool ToolRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
        if (it->name == name) {
            tools_.erase(it);
            return true;
        }
    }
    return false;
}
