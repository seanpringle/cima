#include "tools.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

Tool make_list_directory_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "list_directory";
    t.description = "List files and directories in a given path";
    t.parameters = {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"description", "Directory path to list"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths, tool_logs](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }
        if (!std::filesystem::is_directory(status)) {
            return std::unexpected("Not a directory: " + *resolved);
        }

        std::string result;
        auto it = std::filesystem::directory_iterator(
            *resolved,
            std::filesystem::directory_options::skip_permission_denied,
            ec);
        if (ec) {
            return std::unexpected("Cannot list directory: " + *resolved);
        }
        for (const auto& entry : it) {
            char type = '-';
            if (entry.is_directory())
                type = 'd';
            else if (entry.is_symlink())
                type = 'l';
            result += type;
            result += ' ';
            result += entry.path().filename().string();
            result += '\n';
        }
        if (result.empty()) {
            result = "(empty directory)";
        }

        // Spill to tool_logs if output exceeds threshold
        if (tool_logs) {
            int nl = 0;
            for (char c : result)
                if (c == '\n') nl++;
            if (nl > 100 || result.size() > 4096) {
                size_t id = tool_logs->size() + 1;
                tool_logs->push_back(std::move(result));
                return "(long tool output: " + std::to_string(nl) + " lines, " +
                       std::to_string(tool_logs->back().size()) + " chars. "
                       "Use view_tool_output(id=" + std::to_string(id) + ") to read it)";
            }
        }

        return result;
    };
    return t;
}

Tool make_project_tree_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "project_tree";
    t.description =
        "Recursively list files/directories in a tree-like format.\n"
        "Maximum depth of 5 by default. "
        "Use this to understand project structure in a single call "
        "instead of calling list_directory repeatedly.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description",
                        "Starting directory path (default '.')"}}},
                {"max_depth",
                    {{"type", "integer"},
                        {"description",
                            "Maximum recursion depth (default 5)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum output lines (default 500)"}}}}}};
    t.execute = [safe_dir_ptr, read_only_paths, cancelled, tool_logs](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string("."));
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int max_depth = args.value("max_depth", 5);
        if (max_depth < 1) max_depth = 1;

        int max_lines = args.value("max_lines", 500);
        if (max_lines < 1) max_lines = 1;

        std::error_code ec;
        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }

        // If the path is not a directory, just show it as a single line
        if (!std::filesystem::is_directory(status)) {
            return *resolved + "\n";
        }

        std::string result;
        int line_count = 0;
        bool truncated = false;
        bool interrupted = false;

        // Recursive walk — uses std::function for capture in lambda
        std::function<void(const std::filesystem::path&, int, const std::string&)> walk;
        walk = [&](const std::filesystem::path& dir, int depth, const std::string& prefix) {
            if (depth > max_depth) return;
            if (line_count >= max_lines) {
                truncated = true;
                return;
            }
            if (cancelled && *cancelled) {
                interrupted = true;
                return;
            }

            // Collect directory entries
            std::vector<std::filesystem::directory_entry> entries;
            std::error_code ec2;
            auto it = std::filesystem::directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied, ec2);
            if (ec2) {
                // Cannot read this directory — skip silently
                return;
            }
            auto end = std::filesystem::directory_iterator{};
            for (; it != end; it.increment(ec2)) {
                if (cancelled && *cancelled) {
                    interrupted = true;
                    return;
                }
                if (ec2) {
                    // Permission error on one entry — skip it and continue
                    ec2.clear();
                    continue;
                }
                // Skip .git directory (don't traverse into it)
                if (it->path().filename() == ".git" && it->is_directory()) {
                    continue;
                }
                entries.push_back(*it);
            }

            // Sort: directories first, then files; alphabetically within each group
            std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) {
                    bool a_dir = a.is_directory();
                    bool b_dir = b.is_directory();
                    if (a_dir != b_dir) return a_dir > b_dir; // dirs first
                    // Compare filenames case-insensitively on the stored path
                    return a.path().filename().string() < b.path().filename().string();
                });

            for (size_t i = 0; i < entries.size(); i++) {
                if (cancelled && *cancelled) {
                    interrupted = true;
                    return;
                }
                if (line_count >= max_lines) {
                    truncated = true;
                    return;
                }

                const auto& entry = entries[i];
                bool is_last = (i == entries.size() - 1);

                // Build tree line
                result += prefix;
                result += is_last ? "└── " : "├── ";
                result += entry.path().filename().string();
                if (entry.is_directory()) {
                    result += "/";
                }
                result += "\n";
                line_count++;

                // Recurse into directories
                if (entry.is_directory() && depth < max_depth) {
                    std::string child_prefix = prefix + (is_last ? "    " : "│   ");
                    walk(entry.path(), depth + 1, child_prefix);
                }
            }
        };

        // Root line
        result += *resolved + "/\n";
        line_count++;

        // Walk
        walk(*resolved, 1, "");

        // Spill to tool_logs if output exceeds threshold
        if (tool_logs) {
            int nl = 0;
            for (char c : result)
                if (c == '\n') nl++;
            if (nl > 100 || result.size() > 4096) {
                size_t id = tool_logs->size() + 1;
                tool_logs->push_back(std::move(result));
                return "(long tool output: " + std::to_string(nl) + " lines, " +
                       std::to_string(tool_logs->back().size()) + " chars. "
                       "Use view_tool_output(id=" + std::to_string(id) + ") to read it)";
            }
        }

        if (interrupted) {
            result += "(interrupted)\n";
        } else if (truncated) {
            result += "...(truncated, >" + std::to_string(max_lines) + " lines)\n";
        } else if (line_count <= 1) {
            // Only the root line — directory is empty (or all entries were skipped)
            // Try to detect if the directory actually has no visible entries
            // by counting non-.git items.
            std::error_code ec3;
            int count = 0;
            auto it2 = std::filesystem::directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec3);
            for (; it2 != std::filesystem::directory_iterator{}; it2.increment(ec3)) {
                if (it2->path().filename() != ".git") {
                    count++;
                    if (count > 0) break;
                }
            }
            if (count == 0) {
                result += "(empty directory)\n";
            } else {
                // Some entries exist but were skipped (e.g. permissions)
                // The tree output is already complete with what we could read.
            }
        }

        return result;
    };
    return t;
}

// ── delete_path ─────────────────────────────────────────────────

Tool make_delete_path_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "delete_path";
    t.description = "Delete a file or empty directory. If 'recurse' is "
                    "true, recursively delete a directory and all of its "
                    "contents (use with caution).";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File or directory path to delete"}}},
             {"recurse", {{"type", "boolean"}, {"description", "Recursively delete directory contents"},
                          {"default", false}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        bool recurse = args.value("recurse", false);

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;

        // Check existence first (status() may error on non-existent paths)
        if (!std::filesystem::exists(*resolved, ec)) {
            if (ec) {
                return std::unexpected("Cannot access path: " + *resolved);
            }
            return std::unexpected("Path not found: " + *resolved);
        }

        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }

        if (std::filesystem::is_directory(status)) {
            if (!recurse && !std::filesystem::is_empty(*resolved, ec)) {
                if (ec) {
                    return std::unexpected("Cannot check directory: " + ec.message());
                }
                return std::unexpected("Directory is not empty: " + *resolved);
            }

            if (recurse) {
                std::filesystem::remove_all(*resolved, ec);
            } else {
                std::filesystem::remove(*resolved, ec);
            }
        } else {
            // Regular file (or symlink, etc.) — just remove it
            std::filesystem::remove(*resolved, ec);
        }

        if (ec) {
            return std::unexpected("Failed to delete path: " + ec.message());
        }
        return "ok (deleted " + *resolved + ")";
    };
    return t;
}
