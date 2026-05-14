#include "tools.h"

#include <future>

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

void ToolRegistry::add_defaults(const std::string& safe_dir,
    const std::vector<std::string>& read_only_paths,
    const std::string& search_api_key,
    const std::string& search_engine_id,
    const std::string& search_endpoint,
    bool include_write) {
    add_defaults(std::make_shared<std::string>(safe_dir),
        read_only_paths, search_api_key, search_engine_id,
        search_endpoint, include_write);
}

void ToolRegistry::add_defaults(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    const std::string& search_api_key,
    const std::string& search_engine_id,
    const std::string& search_endpoint,
    bool include_write) {
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
        auto t = make_grep_files_tool(safe_dir_ptr, read_only_paths, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_project_tree_tool(safe_dir_ptr, read_only_paths, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_search_tool(search_api_key, search_engine_id, search_endpoint, cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_web_fetch_tool(cancelled_);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_status_tool(safe_dir_ptr);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_diff_tool(safe_dir_ptr);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }
    {
        auto t = make_git_log_tool(safe_dir_ptr);
        t.permission = ToolPermission::ReadOnly;
        add(std::move(t));
    }

    // ── Write tools ──
    if (include_write) {
        {
            auto t = make_write_file_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_edit_file_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_run_bash_tool(safe_dir_ptr, cancelled_);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_git_add_tool(safe_dir_ptr);
            t.permission = ToolPermission::Write;
            add(std::move(t));
        }
        {
            auto t = make_git_commit_tool(safe_dir_ptr);
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

    return tool->execute(args);
}

Tool* ToolRegistry::find(const std::string& name) {
    for (auto& t : tools_) {
        if (t.name == name)
            return &t;
    }
    return nullptr;
}
