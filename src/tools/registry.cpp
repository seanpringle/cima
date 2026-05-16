#include "tools.h"

#include <future>

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

void ToolRegistry::add_defaults(const std::string& safe_dir,
    const Config& config,
    bool include_write,
    FileModifiedCallback on_file_modified) {
    add_defaults(std::make_shared<std::string>(safe_dir), config, include_write, std::move(on_file_modified));
}

void ToolRegistry::add_defaults(std::shared_ptr<std::string> safe_dir_ptr,
    const Config& config,
    bool include_write,
    FileModifiedCallback on_file_modified) {
    const auto& read_only_paths = config.read_only_paths;

    // ── Read-only tools (receive whitelist for extra path access) ──
    {
        auto t = make_list_files_tool(safe_dir_ptr, read_only_paths);
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
        auto t = make_grep_files_tool(safe_dir_ptr, read_only_paths, config.grep_timeout, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_project_tree_tool(safe_dir_ptr, read_only_paths, config.project_tree_timeout, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_search_tool(config.web_search_timeout, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_fetch_tool(config.web_fetch_timeout, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_status_tool(safe_dir_ptr, config.git_status_timeout);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_diff_tool(safe_dir_ptr, config.git_diff_timeout);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_log_tool(safe_dir_ptr, config.git_log_timeout);
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
            auto t = make_run_bash_tool(safe_dir_ptr, config.bash_timeout, cancelled_);
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
    }

}

json ToolRegistry::to_openai_tools() const {
    return to_openai_tools(nullptr);
}

json ToolRegistry::to_openai_tools(const std::set<std::string>* only_these) const {
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
    std::set<std::string> names;
    for (const auto& t : tools_) {
        if (t.permission == perm)
            names.insert(t.name);
    }
    return names;
}

Result<std::string> ToolRegistry::execute(const std::string& name, const std::string& args_json) {
    Tool* tool = find(name);
    if (!tool) {
        return std::unexpected("unknown tool: " + name);
    }

    json args;
    try {
        args = json::parse(args_json);
    } catch (const json::parse_error& e) {
        return std::unexpected("invalid JSON arguments: " + std::string(e.what()));
    }

    if (tool->timeout_sec > 0) {
        auto future = std::async(
            std::launch::async, [tool, args = std::move(args)] { return tool->execute(args); });
        auto status = future.wait_for(std::chrono::seconds(tool->timeout_sec));
        if (status == std::future_status::timeout) {
            return std::unexpected("tool '" + tool->name + "' timed out after " +
                std::to_string(tool->timeout_sec) + "s");
        }
        return future.get();
    }

    try {
        return tool->execute(args);
    } catch (const std::exception& e) {
        return std::unexpected(
            std::string("tool '") + tool->name + "' threw: " + e.what());
    }
}

Tool* ToolRegistry::find(const std::string& name) {
    for (auto& t : tools_) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}
