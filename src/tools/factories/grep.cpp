#include "tools.h"
#include "tools/text_file_detector.h"

#include <filesystem>
#include <fstream>
#include <git2.h>
#include <regex>
#include <sstream>
#include <string>

// ===================================================================
// grep_files
// ===================================================================

Tool make_grep_files_tool(const Config& config, std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths, int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "grep_files";
    t.description =
        "Search file contents using a regex pattern. "
        "Respects .gitignore rules when searching within a git repository. "
        "Use depth=-1 (default) for unlimited recursion, or set depth=N to limit.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"pattern", {{"type", "string"}, {"description", "Regex pattern to search for"}}},
             {"path",
                 {{"type", "string"},
                  {"description", "File or directory to search in (defaults to .)"}}},
             {"depth",
                 {{"type", "integer"},
                  {"description",
                      "Maximum recursion depth (-1 = unlimited, 0 = root only, default -1)"}}}}},
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

        int max_depth = args.value("depth", -1);

        std::regex re(pattern);
        std::string result;

        auto search_file = [&](const std::filesystem::path& p) {
            std::ifstream file(p);
            if (!file.is_open())
                return;
            std::string line;
            int line_num = 0;
            while (std::getline(file, line)) {
                line_num++;
                try {
                    if (std::regex_search(line, re)) {
                        result += p.string() + ":" + std::to_string(line_num) + ": " + line + '\n';
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
                if (detect_text_file(*resolved) == FileKind::Text) {
                    search_file(*resolved);
                }
            }
        } else if (std::filesystem::is_directory(status)) {
            auto it = std::filesystem::recursive_directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec);
            auto end = std::filesystem::recursive_directory_iterator{};
            for (; it != end; it.increment(ec)) {
                if (cancelled && *cancelled) {
                    result += "(interrupted)";
                    return result;
                }

                // Depth limit
                if (max_depth >= 0 && it.depth() >= max_depth) {
                    it.disable_recursion_pending();
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
                    if (detect_text_file(it->path()) == FileKind::Text) {
                        search_file(it->path());
                    }
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

// ===================================================================
// find_files
// ===================================================================

Tool make_find_files_tool(const Config& config, std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths, int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "find_files";
    t.description =
        "Find files and directories by name using a regex pattern. "
        "Matches against the basename (filename) of each entry, not the full path. "
        "Directory names are listed followed by a '/' character. "
        "Respects .gitignore rules when searching within a git repository.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                 {{"type", "string"},
                  {"description", "Directory path to search in (defaults to .)"}}},
             {"pattern",
                 {{"type", "string"},
                  {"description",
                      "Regex pattern to match against entry basename (filename)"}}},
             {"depth",
                 {{"type", "integer"},
                  {"description",
                      "Maximum recursion depth (1 = immediate children only, default 1)"}}}}},
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

        int max_depth = args.value("depth", 1);
        if (max_depth < 0)
            max_depth = 0;

        std::regex re(pattern);
        std::string result;

        // Try to open a git repository rooted at the search path.
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

        auto safe_dir = *safe_dir_ptr;
        // Normalise safe_dir trailing slash for relative path computation
        if (!safe_dir.empty() && safe_dir.back() != '/')
            safe_dir += '/';

        if (std::filesystem::is_regular_file(status)) {
            // Single file — check name and gitignore
            if (!repo || !is_gitignored(repo, *resolved, repo_workdir)) {
                auto filename = std::filesystem::path(*resolved).filename().string();
                try {
                    if (std::regex_search(filename, re)) {
                        // Show relative path from safe_dir
                        auto rel = std::filesystem::path(*resolved).lexically_relative(
                            std::filesystem::path(safe_dir));
                        result += rel.generic_string() + '\n';
                    }
                } catch (const std::regex_error&) {
                }
            }
        } else if (std::filesystem::is_directory(status)) {
            // Helper to compute relative path and append '/' for dirs
            auto format_entry = [&](const std::filesystem::path& abs_path, bool is_dir) -> std::string {
                auto rel = abs_path.lexically_relative(std::filesystem::path(safe_dir));
                std::string s = rel.generic_string();
                if (is_dir)
                    s += '/';
                return s;
            };

            auto it = std::filesystem::recursive_directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec);
            auto end = std::filesystem::recursive_directory_iterator{};
            for (; it != end; it.increment(ec)) {
                if (cancelled && *cancelled) {
                    if (result.empty())
                        result = "(interrupted)";
                    else
                        result += "(interrupted)";
                    return result;
                }

                // Depth limit
                if (max_depth >= 0 && it.depth() >= max_depth) {
                    it.disable_recursion_pending();
                }

                // Skip .git
                if (it->path().filename() == ".git" && it->is_directory()) {
                    it.disable_recursion_pending();
                    continue;
                }
                // Skip gitignored entries
                if (repo && is_gitignored(repo, it->path(), repo_workdir)) {
                    if (it->is_directory()) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }

                // Check basename against pattern
                auto filename = it->path().filename().string();
                bool matched = false;
                try {
                    matched = std::regex_search(filename, re);
                } catch (const std::regex_error&) {
                    continue;
                }
                if (!matched)
                    continue;

                bool is_dir = it->is_directory();
                result += format_entry(it->path(), is_dir) + '\n';
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
