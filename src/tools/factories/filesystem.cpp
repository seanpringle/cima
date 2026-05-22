#include "tools.h"

#include <filesystem>
#include <functional>
#include <git2.h>
#include <memory>
#include <string>
#include <vector>

// ── list_path ─────────────────────────────────────────────────────

Tool make_list_path_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "list_path";
    t.description =
        "List files and directories in a given path, "
        "recursing up to max_depth (default 1). "
        "Use max_depth > 1 to see nested contents "
        "instead of calling list_path repeatedly.";
    t.timeout_sec = timeout;
    t.parameters = {
        {"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Directory path to list"}}},
             {"max_depth",
                 {{"type", "integer"},
                     {"description",
                         "Maximum recursion depth (default 1)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths, cancelled,
                 tool_logs](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int max_depth = args.value("max_depth", 1);
        if (max_depth < 1) max_depth = 1;

        std::error_code ec;
        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }

        // If the path is a file, show it as a single entry
        if (!std::filesystem::is_directory(status)) {
            std::string result;
            char type = '-';
            if (std::filesystem::is_symlink(*resolved)) type = 'l';
            result += type;
            result += ' ';
            result += std::filesystem::path(*resolved).filename().string();
            result += '\n';
            return result;
        }

        // Open git repo for .gitignore checking
        git_repository* repo = nullptr;
        std::filesystem::path repo_workdir;
        {
            auto repo_res = open_git_repo(*resolved);
            if (repo_res) {
                repo = *repo_res;
                const char* wd = git_repository_workdir(repo);
                if (wd) repo_workdir = std::filesystem::path(wd);
            }
        }
        auto repo_cleanup = std::unique_ptr<git_repository,
            decltype(&git_repository_free)>(repo, git_repository_free);

        std::string result;
        bool interrupted = false;
        auto base_path = std::filesystem::path(*resolved);

        // Recursive walk using std::function
        std::function<void(const std::filesystem::path&, int)> walk;
        walk = [&](const std::filesystem::path& dir, int depth) {
            if (depth > max_depth) return;
            if (cancelled && *cancelled) {
                interrupted = true;
                return;
            }

            std::error_code ec2;
            auto it = std::filesystem::directory_iterator(
                dir,
                std::filesystem::directory_options::skip_permission_denied,
                ec2);
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
                    ec2.clear();
                    continue;
                }

                // Compute relative path from base
                auto rel = it->path().lexically_relative(base_path);
                std::string rel_str = rel.generic_string();

                // When at the base level (depth == 1), use just the filename
                // for backward compatibility
                bool is_dir = it->is_directory();

                char type = 'f';
                if (is_dir)
                    type = 'd';
                else if (it->is_symlink())
                    type = 'l';
                result += type;
                result += ' ';
                result += std::filesystem::path(rel_str).string();

                bool recurse = is_dir;

                if (repo && is_gitignored(repo, it->path(), repo_workdir)) {
                    if (is_dir) {
                        result += " (git ignored, skip auto recurse)";
                    } else {
                        result += " (git ignored)";
                    }
                    recurse = false;
                }

                result += '\n';

                // Recurse into directories
                if (recurse && depth < max_depth) {
                    walk(it->path(), depth + 1);
                }
            }
        };

        walk(*resolved, 1);

        if (interrupted) {
            return "(interrupted)\n";
        }
        if (result.empty()) {
            result = "(empty directory)";
        }
        return spill_long_output(std::move(result), tool_logs);
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
