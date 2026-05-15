#include "tools.h"

#include <filesystem>
#include <fstream>
#include <git2.h>
#include <regex>
#include <string>

// ===================================================================
// grep_files
// ===================================================================

Tool make_grep_files_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled) {
    Tool t;
    t.name = "grep_files";
    t.description = "Search file contents using a regex pattern (max 200 results)";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"pattern", {{"type", "string"}, {"description", "Regex pattern to search for"}}},
                {"path",
                    {{"type", "string"},
                        {"description", "File or directory to search in (defaults to .)"}}}}},
        {"required", {"pattern"}}};
    t.execute = [safe_dir_ptr, read_only_paths, cancelled](const json& args) -> Result<std::string> {
        auto pattern = args.value("pattern", std::string());
        if (pattern.empty()) {
            return std::unexpected(std::string("pattern is required"));
        }

        auto raw_path = args.value("path", std::string("."));
        auto resolved = resolve_path(raw_path, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::regex re(pattern);
        std::string result;
        int count = 0;
        const int max_results = 200;

        auto search_file = [&](const std::filesystem::path& p) {
            std::ifstream file(p);
            if (!file.is_open())
                return;
            std::string line;
            int line_num = 0;
            while (std::getline(file, line) && count < max_results) {
                line_num++;
                try {
                    if (std::regex_search(line, re)) {
                        result += p.string() + ":" + std::to_string(line_num) + ": " + line + '\n';
                        count++;
                    }
                } catch (const std::regex_error&) {
                    // skip lines that cause regex errors
                }
            }
        };

        // Try to open a git repository rooted at the search path.
        // If this fails (not a git repo), repo stays nullptr and gitignore
        // filtering is skipped entirely.
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
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        std::error_code ec;
        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }

        if (std::filesystem::is_regular_file(status)) {
            if (!repo || !is_gitignored(repo, *resolved, repo_workdir)) {
                search_file(*resolved);
            }
        } else if (std::filesystem::is_directory(status)) {
            auto it = std::filesystem::recursive_directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec);
            auto end = std::filesystem::recursive_directory_iterator{};
            for (; it != end && count < max_results; it.increment(ec)) {
                if (cancelled && *cancelled) {
                    break;
                }
                if (it->path().filename() == ".git" && it->is_directory()) {
                    it.disable_recursion_pending();
                    continue;
                }
                // Skip paths matched by .gitignore rules
                if (repo && is_gitignored(repo, it->path(), repo_workdir)) {
                    if (it->is_directory()) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (it->is_regular_file()) {
                    search_file(it->path());
                }
            }
        } else {
            return std::unexpected("Not a file or directory: " + *resolved);
        }

        if (result.empty()) {
            result = "(no matches)";
        }
        return result;
    };
    return t;
}
