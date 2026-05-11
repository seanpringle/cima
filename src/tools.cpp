#include "tools.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <git2.h>

// Initialize libgit2 at module load time.
// git_libgit2_init() is ref-counted and safe to call multiple times.
static const bool g_git_init = (git_libgit2_init(), true);

// Global mutex to serialize git index write operations (git_add, git_commit --all).
// libgit2 acquires a file lock on .git/index.lock when opening the index;
// concurrent index writes from parallel tool calls cause GIT_ELOCKED errors.
static std::mutex g_git_index_mutex;

#include <fcntl.h>
#include <fstream>
#include <future>
#include <mutex>
#include <poll.h>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

// ===================================================================
// Path sandbox
// ===================================================================

Result<std::string> resolve_path(const std::string& raw_path,
    const std::string& safe_dir,
    const std::vector<std::string>& extra_allowed) {
    if (raw_path.empty()) {
        return std::unexpected(std::string("path is required"));
    }

    std::error_code ec;
    std::filesystem::path p(raw_path);

    // For relative paths, resolve against safe_dir first, then normalize
    if (p.is_relative()) {
        p = std::filesystem::path(safe_dir) / p;
    }

    p = std::filesystem::weakly_canonical(p, ec);
    if (ec) {
        p = std::filesystem::path(raw_path).lexically_normal();
        if (p.is_relative()) {
            p = std::filesystem::path(safe_dir) / p;
        }
    }

    std::string resolved = p.string();

    // Normalize safe_dir (no trailing slash)
    auto sd_path = std::filesystem::weakly_canonical(std::filesystem::path(safe_dir), ec);
    if (ec) {
        sd_path = std::filesystem::path(safe_dir).lexically_normal();
    }
    std::string sd = sd_path.string();
    while (!sd.empty() && sd.back() == '/') {
        sd.pop_back();
    }

    if (resolved == sd || resolved.starts_with(sd + "/")) {
        return resolved;
    }

    // Check extra_allowed paths (for read-only tools)
    for (const auto& allowed : extra_allowed) {
        std::string allowed_norm = allowed;
        while (!allowed_norm.empty() && allowed_norm.back() == '/') {
            allowed_norm.pop_back();
        }
        if (resolved == allowed_norm || resolved.starts_with(allowed_norm + "/")) {
            return resolved;
        }
    }

    return std::unexpected("path must be under " + sd);
}

// ===================================================================
// Git helpers
// ===================================================================

// Open the git repository at or walking up from safe_dir.
// Returns the repo handle or an error string.
// Caller must git_repository_free() the handle on success.
static Result<git_repository*> open_git_repo(const std::string& safe_dir) {
    git_repository* repo = nullptr;
    int err = git_repository_open_ext(&repo, safe_dir.c_str(),
        GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        return std::unexpected(
            "not a git repository: " +
            (e ? std::string(e->message) : std::string("unknown error")));
    }
    return repo;
}

// Convert libgit2 status flags to porcelain v1 characters.
// Returns the character for the index (staging area) status.
// Porcelain v1: ' ' = clean, 'M' = modified, 'A' = added, 'D' = deleted,
// 'R' = renamed, 'T' = type change.
// Special case: if the file is untracked (GIT_STATUS_WT_NEW alone),
// both index and worktree show '?'.
static char status_char_for_index(unsigned int flags) {
    if (flags & GIT_STATUS_INDEX_NEW)        return 'A';
    if (flags & GIT_STATUS_INDEX_MODIFIED)   return 'M';
    if (flags & GIT_STATUS_INDEX_DELETED)    return 'D';
    if (flags & GIT_STATUS_INDEX_RENAMED)    return 'R';
    if (flags & GIT_STATUS_INDEX_TYPECHANGE) return 'T';
    // If the only change is untracked in the worktree, show '?' in index too
    if (flags & GIT_STATUS_WT_NEW &&
        !(flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                   GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                   GIT_STATUS_INDEX_TYPECHANGE))) {
        return '?';
    }
    return ' ';
}

// Returns the character for the working tree status.
// Porcelain v1: ' ' = clean, 'M' = modified, 'D' = deleted, '?' = untracked,
// '!' = ignored, 'U' = conflicted.
static char status_char_for_workdir(unsigned int flags) {
    if (flags & GIT_STATUS_WT_NEW)           return '?';
    if (flags & GIT_STATUS_WT_MODIFIED)      return 'M';
    if (flags & GIT_STATUS_WT_DELETED)       return 'D';
    if (flags & GIT_STATUS_WT_RENAMED)       return 'R';
    if (flags & GIT_STATUS_WT_TYPECHANGE)    return 'T';
    if (flags & GIT_STATUS_IGNORED)          return '!';
    if (flags & GIT_STATUS_CONFLICTED)       return 'U';
    return ' ';
}

// ===================================================================
// get_current_git_branch
// ===================================================================

Result<std::string> get_current_git_branch(const std::string& repo_path) {
    auto repo_res = open_git_repo(repo_path);
    if (!repo_res) {
        return std::unexpected(repo_res.error());
    }
    git_repository* repo = *repo_res;
    auto cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
        repo, git_repository_free);

    // Get HEAD reference
    git_reference* head = nullptr;
    int err = git_repository_head(&head, repo);
    if (err != 0) {
        if (err == GIT_EUNBORNBRANCH) {
            return std::string("(unborn branch)");
        }
        if (err == GIT_ENOTFOUND) {
            // No HEAD — empty repository
            return std::unexpected(std::string("no commits yet"));
        }
        const git_error* e = git_error_last();
        return std::unexpected(
            "failed to get HEAD: " +
            (e ? std::string(e->message) : std::string("unknown error")));
    }
    auto head_cleanup = std::unique_ptr<git_reference, decltype(&git_reference_free)>(
        head, git_reference_free);

    // Check if HEAD is a branch or detached
    if (git_reference_is_branch(head)) {
        const char* name = nullptr;
        err = git_branch_name(&name, head);
        if (err != 0 || !name) {
            const git_error* e = git_error_last();
            return std::unexpected(
                "failed to get branch name: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        return std::string(name);
    }

    // Detached HEAD — show short commit hash
    const git_oid* oid = git_reference_target(head);
    if (!oid) {
        return std::string("(detached HEAD)");
    }
    char hex[GIT_OID_HEXSZ + 1];
    git_oid_tostr(hex, sizeof(hex), oid);
    hex[7] = '\0'; // short hash (7 chars)
    return std::string("(detached HEAD at ") + hex + ")";
}

// ===================================================================
// Tool helpers
// ===================================================================

static Tool make_list_files_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "list_files";
    t.description = "List files and directories in a given path";
    t.parameters = {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"description", "Directory path to list"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
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
        return result;
    };
    return t;
}

static Tool make_read_file_lines_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "read_file_lines";
    t.description =
        "Read specific line ranges from a file. Returns lines prefixed with line "
        "numbers. Use this when you know the line numbers you want (e.g. after a "
        "grep match at line 52, read lines 45-78). "
        "For reading from an offset, use read_file instead.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file"}}},
                {"start_line",
                    {{"type", "integer"},
                        {"description",
                            "First line to read (1-indexed, default 1)"}}},
                {"end_line",
                    {{"type", "integer"},
                        {"description",
                            "Last line to read (inclusive). If omitted, reads to "
                            "end of file (capped by max_lines)."}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to return (default 200, max 500)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int start_line = args.value("start_line", 1);
        int end_line = args.value("end_line", 0); // 0 = not specified
        int max_lines = args.value("max_lines", 200);

        if (start_line < 1) {
            return std::unexpected("start_line must be >= 1");
        }
        if (end_line != 0 && end_line < start_line) {
            return std::unexpected("end_line must be >= start_line");
        }
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result;
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before start_line
        while (line_num < start_line - 1 && std::getline(file, line)) {
            line_num++;
        }

        // Determine how many lines to read
        int max_to_read = max_lines;
        if (end_line != 0) {
            int range = end_line - start_line + 1;
            if (range < max_to_read) max_to_read = range;
        }

        while (count < max_to_read && std::getline(file, line)) {
            line_num++;
            result += std::to_string(line_num) + ": " + line + "\n";
            count++;
        }

        // Check if there are more lines beyond what was read
        // Try reading one more line to detect EOF reliably
        bool has_more = false;
        int remaining = 0;
        if (end_line != 0 && line_num < end_line) {
            // We haven't read far enough per end_line — definitely has more
            has_more = true;
            std::string dummy;
            while (std::getline(file, dummy)) { remaining++; }
        } else {
            // Check if there's more content by attempting one more read
            std::string peek;
            has_more = static_cast<bool>(std::getline(file, peek));
            if (has_more) {
                remaining = 1;
                std::string dummy;
                while (std::getline(file, dummy)) { remaining++; }
            }
        }

        if (has_more) {
            result += "...(truncated, >" + std::to_string(count + remaining) +
                " lines from line " + std::to_string(start_line) + ")";
        } else if (count == 0 && start_line > 1) {
            result = "(start_line " + std::to_string(start_line) +
                " is beyond end of file)";
        }
        return result;
    };
    return t;
}

static Tool make_read_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "read_file";
    t.description = "Read lines from a file (max 400 lines at a time, use offset to "
                    "paginate)";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file to read"}}},
                {"offset",
                    {{"type", "integer"},
                        {"description",
                            "Line number to start from (1-indexed, default 0 = "
                            "beginning)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to read starting from offset (default 200)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int offset = args.value("offset", 0);
        int max_lines = args.value("max_lines", 400);
        if (offset < 0)
            offset = 0;
        if (max_lines < 1)
            max_lines = 1;

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result;
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before offset
        while (line_num < offset && std::getline(file, line)) {
            line_num++;
        }

        while (std::getline(file, line) && count < max_lines) {
            line_num++;
            result += line;
            result += '\n';
            count++;
        }

        if (!file.eof()) {
            result += "...(truncated, >" + std::to_string(max_lines) + " lines from offset " +
                std::to_string(offset) + ")";
        } else if (count == 0 && offset > 0) {
            result = "(offset " + std::to_string(offset) + " is beyond end of file)";
        }
        return result;
    };
    return t;
}

// ===================================================================
// Gitignore helpers
// ===================================================================

/// Check whether a path within a git repository is ignored by .gitignore rules.
/// @param repo      An open git_repository*, or nullptr (returns false).
/// @param abs_path  Absolute filesystem path to check.
/// @param workdir   The repository worktree root (from git_repository_workdir()).
static bool is_gitignored(git_repository* repo,
                          const std::filesystem::path& abs_path,
                          const std::filesystem::path& workdir) {
    if (!repo) return false;

    std::error_code ec;
    auto rel = std::filesystem::relative(abs_path, workdir, ec);
    if (ec) return false;

    // Use generic (forward-slash) separators as required by libgit2
    std::string rel_str = rel.generic_string();

    int ignored = 0;
    int err = git_ignore_path_is_ignored(&ignored, repo, rel_str.c_str());
    if (err != 0) {
        // If libgit2 can't check (e.g. path outside the repo), treat as not ignored
        return false;
    }
    return ignored != 0;
}

// ===================================================================
// grep_files
// ===================================================================

static Tool make_grep_files_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    CancellationToken cancelled = nullptr) {
    Tool t;
    t.name = "grep_files";
    t.description = "Search file contents using a regex pattern (max 200 results)";
    t.timeout_sec = 10;
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

static Tool make_write_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "write_file";
    t.description = "Write content to a file, creating parent directories if needed";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path"}}},
                {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
        {"required", {"path", "content"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto content = args.value("content", std::string());

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(*resolved).parent_path(), ec);
        if (ec) {
            return std::unexpected("Failed to create parent directories: " + ec.message());
        }

        std::ofstream file(*resolved, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        file.write(content.data(), content.size());
        file.close();

        return "ok (" + std::to_string(content.size()) + " bytes written)";
    };
    return t;
}

static Tool make_edit_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "edit_file";
    t.description = "Edit a file by searching for an exact string and replacing it. "
                    "The search string must match exactly once in the file — this ensures edits "
                    "are safe and unambiguous. "
                    "Use this to make targeted surgical edits instead of rewriting entire files "
                    "with write_file.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path to edit"}}},
                {"search",
                    {{"type", "string"},
                        {"description",
                            "Exact string to search for; must match exactly once in the file. "
                            "Include surrounding context (unique nearby lines) to guarantee a "
                            "single match."}}},
                {"replace",
                    {{"type", "string"},
                        {"description", "String to replace the matched occurrence with"}}}}},
        {"required", {"path", "search", "replace"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto search = args.value("search", std::string());
        auto replace = args.value("replace", std::string());

        if (search.empty()) {
            return std::unexpected(std::string("search string is required"));
        }

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        // Read the file
        std::ifstream file(*resolved, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Failed to read file: " + *resolved);
        }
        std::string content(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // Count occurrences of the search string
        size_t count = 0;
        size_t pos = 0;
        while ((pos = content.find(search, pos)) != std::string::npos) {
            count++;
            pos += search.size();
        }

        if (count == 0) {
            return std::unexpected("Search string not found in file (0 matches). "
                                   "Use read_file or grep_files to verify the file contents.");
        }
        if (count > 1) {
            return std::unexpected("Search string found " + std::to_string(count) +
                " times in file (expected exactly 1). "
                "Include more surrounding context in the search string to uniquely identify the "
                "location.");
        }

        // Locate the unique occurrence
        pos = content.find(search);

        // Replace the search string with the replacement
        content.replace(pos, search.size(), replace);

        // Write the modified content back to the file
        std::ofstream out(*resolved, std::ios::binary);
        if (!out.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        out.write(content.data(), content.size());
        out.close();

        // Compute the line number where the edit occurred (1-indexed)
        int line_num = 1;
        for (size_t i = 0; i < pos; i++) {
            if (content[i] == '\n')
                line_num++;
        }

        return "ok (replaced 1 occurrence at line " + std::to_string(line_num) + ", " +
            std::to_string(search.size()) + " bytes -> " + std::to_string(replace.size()) +
            " bytes)";
    };
    return t;
}

static Tool make_run_bash_tool(std::shared_ptr<std::string> safe_dir_ptr,
    CancellationToken cancelled = nullptr) {
    Tool t;
    t.name = "run_bash";
    t.description = "Run a bash command in the project directory "
                    "(e.g. build, test, lint). Output is capped at 500 lines / 16000 chars.";
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"command", {{"type", "string"}, {"description", "Shell command to execute"}}}}},
        {"required", {"command"}}};
    t.execute = [safe_dir_ptr, cancelled](const json& args) -> Result<std::string> {
        auto command = args.value("command", std::string());
        if (command.empty()) {
            return std::unexpected(std::string("command is required"));
        }

        // --- fork + exec with pipe and timeout ---
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return std::unexpected(std::string("pipe() failed"));
        }

        pid_t pid = fork();
        if (pid == -1) {
            close(pipefd[0]);
            close(pipefd[1]);
            return std::unexpected(std::string("fork() failed"));
        }

        if (pid == 0) {
            // ---- child ----
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            if (pipefd[1] > STDERR_FILENO)
                close(pipefd[1]);

            setpgid(0, 0); // new process group

            if (!safe_dir_ptr->empty()) {
                if (chdir(safe_dir_ptr->c_str()) != 0) {
                    static const char msg[] = "error: chdir() to safe directory failed\n";
                    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
                    _exit(1);
                }
            }

            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }

        // ---- parent ----
        close(pipefd[1]);
        setpgid(pid, pid); // eliminate race with child's setpgid(0,0)

        std::string output;
        char buf[4096];
        const int timeout_secs = [&] {
            const char* e = std::getenv("LLM_BASH_TIMEOUT");
            return e ? std::atoi(e) : 30;
        }();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

        // Set read end to non-blocking
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags != -1) {
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        }

        auto kill_child = [&] {
            killpg(pid, SIGKILL);
            int st;
            waitpid(pid, &st, 0);
        };

        auto truncate_output = [](std::string& out) {
            if (out.size() > 16000) {
                out = out.substr(0, 16000) + "...(truncated, >16000 chars)";
            }
        };

        while (true) {
            if (cancelled && *cancelled) {
                kill_child();
                close(pipefd[0]);
                truncate_output(output);
                return output + "\n(interrupted)";
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                kill_child();
                close(pipefd[0]);
                truncate_output(output);
                return output;
            }

            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);

            if (n > 0) {
                buf[n] = '\0';
                output += buf;
                if (output.size() > 16000) {
                    kill_child();
                    close(pipefd[0]);
                    output = output.substr(0, 16000) + "...(truncated, >16000 chars)";
                    return output;
                }
            } else if (n == 0) {
                break; // EOF
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {pipefd[0], POLLIN, 0};
                auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                if (remaining > 0) {
                    poll(&pfd, 1, std::min(remaining, 100L));
                }
            } else {
                break;
            }
        }

        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);

        // Line count truncation
        int nl = 0;
        for (char c : output)
            if (c == '\n')
                nl++;
        if (nl > 500) {
            size_t pos = 0;
            for (int i = 0; i < 500; i++) {
                pos = output.find('\n', pos) + 1;
            }
            output = output.substr(0, pos) + "...(truncated, >500 lines)";
        }

        // Final size cap
        truncate_output(output);

        return output;
    };
    return t;
}

// ===================================================================
// Web search tool (libcurl)
// ===================================================================

static size_t web_search_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static int web_search_progress_cb(void* clientp,
    curl_off_t /*dltotal*/,
    curl_off_t /*dlnow*/,
    curl_off_t /*ultotal*/,
    curl_off_t /*ulnow*/) {
    auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
    return (cancelled && *cancelled) ? 1 : 0;
}

// ── DuckDuckGo rate limiter ──
// Enforces a minimum gap between successive requests to the free DDG API.
static std::chrono::steady_clock::time_point g_last_ddg_request;
static std::mutex g_ddg_mutex;
static constexpr auto DDG_MIN_INTERVAL = std::chrono::milliseconds(1000);

// ── Shared HTTP GET helper (used by web_search and web_fetch) ──
// Returns (body, http_code) or an error string.
static Result<std::pair<std::string, long>> http_get(const std::string& url, int timeout_sec = 15,
    std::atomic<bool>* cancelled = nullptr) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return std::unexpected("curl_easy_init failed");

    std::string body;
    long http_code = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Use system default CA bundle — do NOT set a custom CA path so that
    // curl finds the system trust store automatically.
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return std::unexpected(std::string("web_search curl error: ") +
            curl_easy_strerror(res));
    }
    return std::make_pair(std::move(body), http_code);
}

static Tool make_web_search_tool(const std::string& api_key,
    const std::string& engine_id,
    const std::string& endpoint_override,
    CancellationToken cancelled = nullptr) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web. Returns up to 10 results with titles, snippets, and URLs. "
        "By default uses the DuckDuckGo Instant Answer API (no key required). "
        "To use Google Custom Search, set SEARCH_API_KEY + SEARCH_ENGINE_ID. "
        "For a custom endpoint, set SEARCH_ENDPOINT with a {query} placeholder.";
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"query",
                {{"type", "string"}, {"description", "Search query (max 500 characters)"}}}}},
        {"required", {"query"}}};
    t.execute = [api_key, engine_id, endpoint_override, cancelled](
                    const json& args) -> Result<std::string> {
        auto query = args.value("query", std::string());
        if (query.empty())
            return std::unexpected("query is required");

        if (query.size() > 500)
            query = query.substr(0, 500);

        // Determine which backend to use
        bool use_google = !api_key.empty() && !engine_id.empty();
        bool use_custom = !endpoint_override.empty();
        bool use_ddg = !use_google && !use_custom;

        // Build the request URL
        std::string url;
        std::string response_format_hint; // "google", "duckduckgo", "custom"
        if (use_custom) {
            response_format_hint = "custom";
            url = endpoint_override;
            auto pos = url.find("{query}");
            if (pos != std::string::npos) {
                char* encoded = curl_easy_escape(nullptr, query.c_str(), (int)query.size());
                if (!encoded)
                    return std::unexpected("curl_easy_escape failed");
                url.replace(pos, 7, encoded);
                curl_free(encoded);
            }
        } else if (use_google) {
            response_format_hint = "google";
            char* enc_key = curl_easy_escape(nullptr, api_key.c_str(), 0);
            char* enc_cx = curl_easy_escape(nullptr, engine_id.c_str(), 0);
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_key || !enc_cx || !enc_q) {
                curl_free(enc_key);
                curl_free(enc_cx);
                curl_free(enc_q);
                return std::unexpected("curl_easy_escape failed");
            }
            url = "https://www.googleapis.com/customsearch/v1?key=" +
                std::string(enc_key) + "&cx=" + std::string(enc_cx) + "&q=" + std::string(enc_q);
            curl_free(enc_key);
            curl_free(enc_cx);
            curl_free(enc_q);
        } else {
            // DuckDuckGo Instant Answer API — no API key required
            response_format_hint = "duckduckgo";
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_q)
                return std::unexpected("curl_easy_escape failed");
            url = "https://api.duckduckgo.com/?q=" +
                std::string(enc_q) + "&format=json&no_html=1&skip_disambig=1";
            curl_free(enc_q);

            // Rate-limit: DDG free API requires ~1s between requests
            {
                std::lock_guard<std::mutex> lock(g_ddg_mutex);
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_last_ddg_request);
                if (elapsed < DDG_MIN_INTERVAL) {
                    std::this_thread::sleep_for(DDG_MIN_INTERVAL - elapsed);
                }
                g_last_ddg_request = std::chrono::steady_clock::now();
            }

            // Retry loop with exponential backoff on HTTP 429
            std::string body;
            long http_code = 0;
            int max_retries = 3;
            int delay_ms = 1000;
            for (int attempt = 0; attempt <= max_retries; attempt++) {
                auto resp = http_get(url, 15, cancelled.get());
                if (!resp) {
                    if (attempt == max_retries)
                        return std::unexpected(resp.error());
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    delay_ms *= 2;
                    continue;
                }
                body = resp->first;
                http_code = resp->second;
                if (http_code != 429)
                    break;
                if (attempt < max_retries) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    delay_ms *= 2;
                }
            }

            if (http_code != 200) {
                std::string msg = "web_search HTTP " + std::to_string(http_code);
                if (!body.empty())
                    msg += ": " + body.substr(0, 500);
                return std::unexpected(msg);
            }

            // Parse JSON response
            json j;
            try {
                j = json::parse(body);
            } catch (const json::parse_error& e) {
                return std::unexpected("web_search JSON parse error: " + std::string(e.what()));
            }

            // Format DuckDuckGo Instant Answer response
            // See: https://duckduckgo.com/api
            {
                std::string result;

                // Direct answer (e.g. "12 months" for "months in a year")
                if (!j.value("Answer", "").empty()) {
                    result += "Answer: " + j["Answer"].get<std::string>() + "\n\n";
                }

                // Abstract/summary
                std::string abstract = j.value("AbstractText", "");
                if (!abstract.empty()) {
                    result += abstract + "\n";
                    std::string src = j.value("AbstractSource", "");
                    std::string src_url = j.value("AbstractURL", "");
                    if (!src.empty())
                        result += "Source: " + src + " (" + src_url + ")\n";
                    result += "\n";
                }

                // Definition
                std::string def = j.value("Definition", "");
                if (!def.empty()) {
                    result += "Definition: " + def + "\n\n";
                }

                // Heading (the topic of the instant answer)
                std::string heading = j.value("Heading", "");
                if (!heading.empty() && abstract.empty()) {
                    result += "Topic: " + heading + "\n\n";
                }

                // Results array (top 5)
                if (j.contains("Results") && j["Results"].is_array()) {
                    for (const auto& item : j["Results"]) {
                        std::string text = item.value("Text", "");
                        std::string link = item.value("FirstURL", "");
                        if (!text.empty()) {
                            result += "- " + text + "\n";
                            if (!link.empty()) result += "  " + link + "\n";
                            result += "\n";
                        }
                    }
                }

                // RelatedTopics (flattening subcategories)
                if (j.contains("RelatedTopics") && j["RelatedTopics"].is_array()) {
                    for (const auto& topic : j["RelatedTopics"]) {
                        if (topic.contains("Topics") && topic["Topics"].is_array()) {
                            // Nested subcategory
                            for (const auto& sub : topic["Topics"]) {
                                std::string text = sub.value("Text", "");
                                std::string link = sub.value("FirstURL", "");
                                if (!text.empty()) {
                                    result += "- " + text + "\n";
                                    if (!link.empty()) result += "  " + link + "\n";
                                    result += "\n";
                                }
                            }
                        } else {
                            std::string text = topic.value("Text", "");
                            std::string link = topic.value("FirstURL", "");
                            if (!text.empty()) {
                                result += "- " + text + "\n";
                                if (!link.empty()) result += "  " + link + "\n";
                                result += "\n";
                            }
                        }
                    }
                }

                if (result.empty())
                    return std::string("(no results found)");
                return result;
            }
        }

        // =====================================================================
        // Google CSE and custom endpoint share the same HTTP + parsing logic
        // =====================================================================

        // libcurl GET
        CURL* curl = curl_easy_init();
        if (!curl)
            return std::unexpected("curl_easy_init failed");

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled.get());

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_search curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_search HTTP " + std::to_string(http_code);
            if (!body.empty())
                msg += ": " + body.substr(0, 500);
            return std::unexpected(msg);
        }

        // Parse JSON response
        json j;
        try {
            j = json::parse(body);
        } catch (const json::parse_error& e) {
            return std::unexpected("web_search JSON parse error: " + std::string(e.what()));
        }

        // Google CSE or custom endpoint: expects {"items": [...]}
        if (!j.contains("items") || !j["items"].is_array() || j["items"].empty()) {
            return std::string("(no results found)");
        }
        std::string result;
        int rank = 1;
        for (const auto& item : j["items"]) {
            if (rank > 10)
                break;
            std::string title = item.value("title", "(no title)");
            std::string snippet = item.value("snippet", "");
            std::string link = item.value("link", "");
            result += std::to_string(rank) + ". " + title + "\n";
            if (!snippet.empty())
                result += "   " + snippet + "\n";
            if (!link.empty())
                result += "   " + link + "\n";
            result += "\n";
            rank++;
        }
        return result;
    };
    return t;
}

// ===================================================================
// Web fetch tool (libcurl)
// ===================================================================

// ── URL validation helper ──

// Returns true if the scheme is http or https (case-insensitive).
// Rejects file://, ftp://, data:, javascript:, etc.
static bool is_valid_fetch_scheme(const std::string& url) {
    auto pos = url.find(':');
    if (pos == std::string::npos) return false;
    std::string scheme = url.substr(0, pos);
    for (auto& c : scheme) c = char(std::tolower((unsigned char)c));
    return scheme == "http" || scheme == "https";
}

// ── Caching ──

static std::mutex g_fetch_cache_mutex;
static std::unordered_map<std::string, std::string> g_fetch_cache;

// ── Tool factory ──

static Tool make_web_fetch_tool(CancellationToken cancelled = nullptr) {
    Tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch the content of a URL. Returns the response body as text. "
        "Max 100,000 characters. "
        "Use this to read documentation, API references, or web pages. "
        "Only returns the raw response body; for search results use web_search.";
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"url",
                {{"type", "string"},
                    {"description",
                        "URL to fetch (http/https). "
                        "Results are cached per session — re-fetching the same URL "
                        "returns the cached content."}}}}},
        {"required", {"url"}}};
    t.execute = [cancelled](const json& args) -> Result<std::string> {
        auto url = args.value("url", std::string());
        if (url.empty()) {
            return std::unexpected("url is required");
        }

        // ── URL validation ──
        // 1. Check scheme (http/https only)
        if (!is_valid_fetch_scheme(url)) {
            return std::unexpected("web_fetch: only http and https URLs are supported");
        }

        // ── Cache check ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            auto it = g_fetch_cache.find(url);
            if (it != g_fetch_cache.end()) {
                return it->second;
            }
        }

        // ── HTTP GET via libcurl ──
        CURL* curl = curl_easy_init();
        if (!curl) {
            return std::unexpected("web_fetch: curl_easy_init failed");
        }

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled.get());

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        // Read Content-Type BEFORE curl_easy_cleanup — the pointer is only
        // valid while the handle lives.
        std::string content_type;
        char* ct_raw = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_raw);
        if (ct_raw != nullptr) {
            content_type = ct_raw;
        }

        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_fetch curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_fetch HTTP " + std::to_string(http_code);
            if (!body.empty()) {
                msg += ": " + body.substr(0, 500);
            }
            return std::unexpected(msg);
        }

        // ── Content-Type filtering ──
        // Only allow text-based content types. If the server didn't send a
        // Content-Type header, we allow it (assume text).
        if (!content_type.empty()) {
            std::string ct = content_type;
            // Convert to lowercase for comparison
            for (auto& c : ct) c = char(std::tolower((unsigned char)c));

            // Strip parameters like charset=utf-8
            auto semi = ct.find(';');
            if (semi != std::string::npos) ct = ct.substr(0, semi);

            // Trim trailing whitespace
            while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t'))
                ct.pop_back();

            // Allowed content types
            bool allowed = false;
            if (ct.find("text/") == 0) allowed = true;
            else if (ct == "application/json") allowed = true;
            else if (ct == "application/xml") allowed = true;
            else if (ct == "application/javascript") allowed = true;
            else if (ct == "application/x-javascript") allowed = true;
            else if (ct == "application/atom+xml") allowed = true;
            else if (ct == "application/rss+xml") allowed = true;
            else if (ct == "application/xhtml+xml") allowed = true;

            if (!allowed) {
                return std::unexpected(
                    "web_fetch: unsupported Content-Type '" + ct +
                    "' — only text-based content can be fetched");
            }
        }

        // ── Truncation ──
        const size_t max_chars = 100000;
        if (body.size() > max_chars) {
            body = body.substr(0, max_chars) +
                "\n...(truncated, >" + std::to_string(max_chars) + " chars)";
        }

        // ── Cache the result ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            g_fetch_cache[url] = body;
        }

        return body;
    };
    return t;
}

// ===================================================================
// git_status tool
// ===================================================================

static Tool make_git_status_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "git_status";
    t.description =
        "Return the working tree status in short format (like 'git status --short').\n"
        "Each line uses the two-character porcelain format:\n"
        "  XY <path>\n"
        "where X is the index status and Y is the working tree status.\n"
        "  ' ' = unmodified, M = modified, A = added, D = deleted, "
        "R = renamed, C = copied, U = updated, ? = untracked, ! = ignored\n"
        "Output is sorted by path and capped at 200 entries.";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties", json::object()},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr](const json& /*args*/) -> Result<std::string> {
        // Open repo
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        // Collect status entries
        struct Entry {
            std::string path;
            char x; // index status
            char y; // working tree status
        };
        std::vector<Entry> entries;
        const int max_entries = 200;

        git_status_options opts = GIT_STATUS_OPTIONS_INIT;
        opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
                   | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
                   | GIT_STATUS_OPT_INCLUDE_IGNORED
                   | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

        struct CbPayload {
            std::vector<Entry>* out;
            int max;
        };
        CbPayload payload{&entries, max_entries};

        auto cb = [](const char* path, unsigned int status_flags, void* data) -> int {
            auto* p = static_cast<CbPayload*>(data);
            if (static_cast<int>(p->out->size()) >= p->max)
                return 1; // abort iteration

            Entry e;
            e.path = path ? path : "";
            e.x = status_char_for_index(status_flags);
            e.y = status_char_for_workdir(status_flags);
            p->out->push_back(std::move(e));
            return 0; // continue
        };

        int err = git_status_foreach_ext(repo, &opts, cb, &payload);
        if (err < 0 && err != GIT_EUSER) {
            const git_error* e = git_error_last();
            return std::unexpected("git_status error: " +
                (e ? std::string(e->message) : std::string("unknown")));
        }

        // Sort by path
        std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.path < b.path; });

        // Format output
        std::string result;
        bool truncated = (entries.size() >= static_cast<size_t>(max_entries));
        size_t count = truncated ? max_entries : entries.size();
        for (size_t i = 0; i < count; i++) {
            const auto& e = entries[i];
            result += e.x;
            result += e.y;
            result += ' ';
            result += e.path;
            result += '\n';
        }
        if (truncated) {
            result += "...(truncated, >" + std::to_string(entries.size()) + " files)";
        }
        if (result.empty()) {
            result = "(clean — no changes)";
        }
        return result;
    };
    return t;
}

// ===================================================================
// git_diff tool
// ===================================================================

static Tool make_git_diff_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "git_diff";
    t.description =
        "Return a unified diff of unstaged (or staged) changes.\n"
        "Output is capped at 500 lines / 16000 chars.\n"
        "Use git_status first to see which files have changed, "
        "then git_diff to inspect the actual changes.\n"
        "If 'staged' is true, shows the diff that would be committed "
        "(index vs HEAD). If false (default), shows unstaged changes "
        "(working tree vs index).";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"staged",
                {{"type", "boolean"},
                    {"description",
                        "If true, show staged changes (diff of index vs HEAD). "
                        "If false (default), show unstaged changes "
                        "(diff of working tree vs index)."}}},
                {"path",
                    {{"type", "string"},
                        {"description",
                            "Optional file path to limit the diff to a specific file. "
                            "If provided, only this file's changes are shown. "
                            "Must be under the safe directory."}}}}},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        // Open git repository
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        bool staged = args.value("staged", false);
        std::string filter_path = args.value("path", std::string());

        // Validate path if provided
        if (!filter_path.empty()) {
            auto resolved = resolve_path(filter_path, *safe_dir_ptr);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            // resolve_path gives an absolute path; for libgit2 pathspec we need
            // a path relative to the repo working directory. We try to make it
            // relative, but absolute paths also work as long as they're under
            // the workdir. We'll use the raw user input as-is; libgit2 handles
            // normalization relative to the workdir root.
        }

        // Build diff options with optional pathspec
        git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
        const char* paths_arr[1];
        if (!filter_path.empty()) {
            paths_arr[0] = filter_path.c_str();
            diff_opts.pathspec.strings = const_cast<char**>(paths_arr);
            diff_opts.pathspec.count = 1;
        }

        git_diff* diff = nullptr;
        int err = 0;

        if (staged) {
            // Staged changes: diff HEAD tree vs index
            git_object* head_tree_obj = nullptr;
            err = git_revparse_single(&head_tree_obj, repo, "HEAD^{tree}");
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_diff error resolving HEAD: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
            git_tree* head_tree = reinterpret_cast<git_tree*>(head_tree_obj);

            err = git_diff_tree_to_index(&diff, repo, head_tree, nullptr, &diff_opts);
            git_tree_free(head_tree);
        } else {
            // Unstaged changes: diff index vs working directory
            err = git_diff_index_to_workdir(&diff, repo, nullptr, &diff_opts);
        }

        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_diff error: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        auto diff_cleanup = std::unique_ptr<git_diff, decltype(&git_diff_free)>(
            diff, git_diff_free);

        // Print unified diff into a string, capped at 500 lines / 16000 chars
        std::string result;
        const int max_lines = 500;
        const size_t max_chars = 16000;

        struct DiffCtx {
            std::string* output;
            int line_count = 0;
            bool truncated = false;
        };
        DiffCtx ctx{&result, 0, false};

        auto print_cb = [](const git_diff_delta* /*delta*/,
                           const git_diff_hunk* /*hunk*/,
                           const git_diff_line* line,
                           void* payload) -> int {
            auto* c = static_cast<DiffCtx*>(payload);
            if (c->truncated) return 1;
            if (c->line_count >= max_lines || c->output->size() >= max_chars) {
                c->truncated = true;
                return 1;
            }
            // Prepend origin character for +/-/context lines
            if (line->origin == '+' || line->origin == '-' || line->origin == ' ') {
                c->output->push_back(line->origin);
            }
            c->output->append(line->content, line->content_len);
            c->line_count++;
            return 0;
        };

        err = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, print_cb, &ctx);
        if (err && !ctx.truncated) {
            // Only report as error if we didn't intentionally stop via truncation
            const git_error* e = git_error_last();
            return std::unexpected("git_diff print error: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        // Append truncation trailer if needed
        if (ctx.truncated) {
            result += "\n...(diff truncated at " + std::to_string(max_lines) +
                " lines / " + std::to_string(max_chars) + " chars)";
        }

        if (result.empty()) {
            result = staged ? "(no staged changes)" : "(no unstaged changes)";
        }

        return result;
    };
    return t;
}

// ===================================================================
// git_log tool
// ===================================================================

static Tool make_git_log_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "git_log";
    t.description =
        "Return recent commit history.\n"
        "Output formats:\n"
        "  'short' (default): commit hash, author, date, subject\n"
        "  'oneline': <hash_prefix> <subject>\n"
        "  'full': commit hash, author, date, and full message body\n"
        "Use 'branch' to specify a revision (branch, tag, commit hash, HEAD~N, etc.).\n"
        "Defaults to HEAD (current branch tip).";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"max_count",
                {{"type", "integer"},
                    {"description", "Maximum number of commits to return (default 10, max 50)"}}},
             {"format",
                {{"type", "string"},
                    {"enum", {"oneline", "short", "full"}},
                    {"description", "Output format: oneline, short (default), or full"}}},
             {"branch",
                {{"type", "string"},
                    {"description", "Git revision to start from (e.g. 'main', 'HEAD~3', 'v1.0'). "
                                   "Defaults to HEAD."}}}}},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        // Open repo
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        int max_count = args.value("max_count", 10);
        if (max_count < 1) max_count = 1;
        if (max_count > 50) max_count = 50;

        std::string format = args.value("format", std::string("short"));
        std::string branch = args.value("branch", std::string());

        // Validate format
        if (format != "oneline" && format != "short" && format != "full") {
            return std::unexpected("git_log error: invalid format '" + format +
                "'. Must be 'oneline', 'short', or 'full'.");
        }

        // Create revwalk
        git_revwalk* walk = nullptr;
        int err = git_revwalk_new(&walk, repo);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_log error creating revwalk: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        auto walk_cleanup = std::unique_ptr<git_revwalk, decltype(&git_revwalk_free)>(
            walk, git_revwalk_free);

        // Sort by time (newest first)
        git_revwalk_sorting(walk, GIT_SORT_TIME);

        // Push start revision
        if (!branch.empty()) {
            git_object* obj = nullptr;
            err = git_revparse_single(&obj, repo, branch.c_str());
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_log error resolving '" + branch + "': " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
            auto obj_cleanup = std::unique_ptr<git_object, decltype(&git_object_free)>(
                obj, git_object_free);

            if (git_object_type(obj) != GIT_OBJECT_COMMIT) {
                return std::unexpected("git_log error: '" + branch + "' is not a commit");
            }

            err = git_revwalk_push(walk, git_object_id(obj));
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_log error pushing revision: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        } else {
            err = git_revwalk_push_head(walk);
            if (err) {
                // No HEAD (empty repo, unborn branch, etc.)
                const git_error* e = git_error_last();
                return std::unexpected("git_log error: " +
                    (e ? std::string(e->message) : std::string("no commits found")));
            }
        }

        // Walk commits
        std::string result;
        int count = 0;
        const size_t max_chars = 16000;

        git_oid oid;
        while (git_revwalk_next(&oid, walk) == 0 && count < max_count) {
            // Check output size cap (safety valve)
            if (result.size() >= max_chars) {
                result += "...(output truncated at " + std::to_string(max_chars) + " chars)";
                break;
            }

            git_commit* commit = nullptr;
            err = git_commit_lookup(&commit, repo, &oid);
            if (err) continue; // skip corrupt commits

            auto commit_cleanup = std::unique_ptr<git_commit, decltype(&git_commit_free)>(
                commit, git_commit_free);

            // Format commit hash
            char hex[GIT_OID_HEXSZ + 1];
            git_oid_tostr(hex, sizeof(hex), &oid);

            // Get author
            const git_signature* author = git_commit_author(commit);
            std::string author_str = "unknown <unknown>";
            if (author) {
                author_str = (author->name ? author->name : "unknown");
                author_str += " <";
                author_str += (author->email ? author->email : "unknown");
                author_str += ">";
            }

            // Format commit date/time
            time_t commit_time = git_commit_time(commit);
            int time_offset = git_commit_time_offset(commit); // minutes from UTC

            // Apply offset to UTC to get local time
            time_t local_epoch = commit_time + time_offset * 60;
            struct tm local_tm{};
            gmtime_r(&local_epoch, &local_tm);

            char date_buf[64];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &local_tm);

            char tz_buf[16];
            int offset_hours = time_offset / 60;
            int offset_mins = std::abs(time_offset) % 60;
            char sign = (time_offset >= 0) ? '+' : '-';
            snprintf(tz_buf, sizeof(tz_buf), " %c%02d%02d", sign,
                     std::abs(offset_hours), offset_mins);

            std::string date_str = std::string(date_buf) + tz_buf;

            // Get commit message
            const char* raw_msg = git_commit_message(commit);
            std::string msg(raw_msg ? raw_msg : "");

            // Extract subject (first line) and body
            std::string subject = msg;
            std::string body;
            auto nl = msg.find('\n');
            if (nl != std::string::npos) {
                subject = msg.substr(0, nl);
                size_t body_start = nl + 1;
                // Skip leading blank lines in body
                while (body_start < msg.size() &&
                       (msg[body_start] == '\n' || msg[body_start] == '\r')) {
                    body_start++;
                }
                if (body_start < msg.size()) {
                    body = msg.substr(body_start);
                    // Remove trailing newlines
                    while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
                        body.pop_back();
                }
            }

            // Format output
            if (format == "oneline") {
                // <8-char hash> <subject>
                std::string short_hash(hex, 8);
                result += short_hash + " " + subject + "\n";
            } else if (format == "full") {
                result += "commit " + std::string(hex) + "\n";
                result += "Author: " + author_str + "\n";
                result += "Date:   " + date_str + "\n\n";

                // Full message indented by 4 spaces (including subject + body)
                std::string full_msg = msg;
                // Remove trailing newlines
                while (!full_msg.empty() &&
                       (full_msg.back() == '\n' || full_msg.back() == '\r'))
                    full_msg.pop_back();

                if (!full_msg.empty()) {
                    size_t pos = 0;
                    while (pos < full_msg.size()) {
                        result += "    ";
                        auto next = full_msg.find('\n', pos);
                        if (next == std::string::npos) {
                            result += full_msg.substr(pos) + "\n";
                            break;
                        }
                        result += full_msg.substr(pos, next - pos) + "\n";
                        pos = next + 1;
                    }
                }
                result += "---\n";
            } else {
                // short format (default)
                result += "commit " + std::string(hex) + "\n";
                result += "Author: " + author_str + "\n";
                result += "Date:   " + date_str + "\n\n";
                result += "    " + subject + "\n";
                result += "---\n";
            }

            count++;
        }

        if (result.empty()) {
            return std::string("(no commits)");
        }

        // Remove trailing separator
        if (result.size() >= 4 && result.substr(result.size() - 4) == "---\n") {
            result.resize(result.size() - 4);
        }

        return result;
    };
    return t;
}

// ===================================================================
// git_add tool
// ===================================================================

static Tool make_git_add_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "git_add";
    t.description =
        "Stage file(s) for commit. Like 'git add <path>' or 'git add -A'.\n"
        "If 'all' is true, stages all changes (added, modified, deleted) "
        "in the entire working tree.\n"
        "If 'path' is specified, only that file or pathspec is staged.\n"
        "Use git_status first to see which files are changed, "
        "then git_add to stage them, then git_commit to commit.";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"oneOf",
                    {{{"type", "string"},
                      {"description",
                          "File path or pathspec to stage. "
                          "Must be under the safe directory."}},
                     {{"type", "array"},
                      {"items", {{"type", "string"}}},
                      {"description",
                          "Array of file paths to stage. "
                          "Each must be under the safe directory."}}}},
                 {"description",
                     "File path, pathspec, or array of paths to stage. "
                     "Defaults to '.' (current directory)."}}},
             {"all",
                {{"type", "boolean"},
                    {"description",
                        "If true, stage all changes (added, modified, deleted). "
                        "Like 'git add -A'. Default false."}}}}},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        // Serialize with other git index write operations
        std::lock_guard<std::mutex> lock(g_git_index_mutex);

        // Open git repository
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        bool all = args.value("all", false);

        // Get the repository's index
        git_index* index = nullptr;
        int err = git_repository_index(&index, repo);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_add error opening index: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        auto index_cleanup = std::unique_ptr<git_index, decltype(&git_index_free)>(
            index, git_index_free);

        if (all) {
            // Stage all changes (git add -A)
            // Use NULL pathspec to cover the entire working tree
            err = git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_add error staging all changes: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        } else {
            // Helper lambda to resolve a path and stage it via git_index_add_bypath
            auto stage_single = [&](const std::string& path_str) -> Result<void> {
                auto resolved = resolve_path(path_str, *safe_dir_ptr);
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }

                // Convert the absolute resolved path to a path relative to the
                // repository's working directory, as required by git_index_add_bypath.
                const char* workdir_raw = git_repository_workdir(repo);
                if (!workdir_raw) {
                    return std::unexpected("git_add: repository has no working directory");
                }
                std::string workdir(workdir_raw);
                // Normalize: ensure trailing slash for prefix matching
                if (!workdir.empty() && workdir.back() != '/') workdir += '/';

                std::string abs_path = *resolved;
                if (abs_path.size() > workdir.size() &&
                    abs_path.compare(0, workdir.size(), workdir) == 0) {
                    // Path is under workdir — make relative
                    std::string rel_path = abs_path.substr(workdir.size());
                    if (rel_path.empty()) {
                        return std::unexpected("git_add: cannot stage the repository root");
                    }

                    err = git_index_add_bypath(index, rel_path.c_str());
                    if (err) {
                        const git_error* e = git_error_last();
                        return std::unexpected("git_add error staging '" + rel_path + "': " +
                            (e ? std::string(e->message) : std::string("unknown error")));
                    }
                    return {};
                } else {
                    return std::unexpected("git_add: path is outside the repository working directory");
                }
            };

            // Accept path as either a single string or an array of strings
            auto path_it = args.find("path");
            if (path_it != args.end() && path_it->is_array()) {
                // Array of paths
                std::vector<std::string> staged;
                for (const auto& p : *path_it) {
                    if (!p.is_string()) {
                        return std::unexpected("git_add: each path in the array must be a string");
                    }
                    auto result = stage_single(p.get<std::string>());
                    if (!result) {
                        return std::unexpected(result.error());
                    }
                    staged.push_back(p.get<std::string>());
                }
                if (staged.empty()) {
                    return std::unexpected("git_add: path array is empty");
                }
                // Write the index after the last addition
                err = git_index_write(index);
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_add error writing index: " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                }

                std::string msg = "ok (staged " + std::to_string(staged.size()) + " files: ";
                for (size_t i = 0; i < staged.size(); i++) {
                    if (i > 0) msg += ", ";
                    msg += staged[i];
                }
                msg += ")";
                return msg;
            } else {
                // Single path (default to ".")
                std::string path = args.value("path", std::string("."));
                auto result = stage_single(path);
                if (!result) {
                    return std::unexpected(result.error());
                }

                // Write the index to persist changes
                err = git_index_write(index);
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_add error writing index: " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                }

                return std::string("ok (staged " + path + ")");
            }
        }

        // Write the index to persist changes (all: true path)
        err = git_index_write(index);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_add error writing index: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        return std::string("ok (staged all changes)");
    };
    return t;
}

// ===================================================================
// git_commit tool
// ===================================================================

static Tool make_git_commit_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "git_commit";
    t.description =
        "Create a new commit with staged changes. Like 'git commit -m <message>'.\n"
        "If 'all' is true, stages all changes before committing (like 'git commit -a').\n"
        "Use git_status first to see which files are changed, "
        "then git_add to stage them, then git_commit to commit.";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"message",
                {{"type", "string"},
                    {"description", "Commit message."}}},
             {"all",
                {{"type", "boolean"},
                    {"description",
                        "If true, stage all changes before committing. "
                        "Like 'git commit -a'. Default false."}}}}},
        {"required", {"message"}}};

    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        // Serialize with other git index write operations
        std::lock_guard<std::mutex> lock(g_git_index_mutex);

        auto message = args.value("message", std::string());
        if (message.empty()) {
            return std::unexpected("commit message is required");
        }

        bool all = args.value("all", false);

        // Open git repository
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        // If --all, stage all changes first
        if (all) {
            git_index* index = nullptr;
            int err = git_repository_index(&index, repo);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_commit error opening index: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
            auto index_cleanup = std::unique_ptr<git_index, decltype(&git_index_free)>(
                index, git_index_free);

            err = git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_commit error staging all changes: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }

            err = git_index_write(index);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_commit error writing index: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        }

        // Create the commit from the staging area
        git_oid commit_oid;
        git_commit_create_options opts = GIT_COMMIT_CREATE_OPTIONS_INIT;
        // allow_empty_commit defaults to 0 (false) — this matches git behavior

        int err = git_commit_create_from_stage(
            &commit_oid, repo, message.c_str(), &opts);
        if (err) {
            const git_error* e = git_error_last();
            if (err == GIT_EUNCHANGED) {
                return std::unexpected("git_commit: no changes to commit. "
                    "Use 'all: true' or stage changes first with git_add.");
            }
            return std::unexpected("git_commit error: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        // Format short hash for confirmation
        char hex[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hex, sizeof(hex), &commit_oid);
        std::string short_hash(hex, 8);

        return "ok (committed as " + short_hash + ": " + message + ")";
    };
    return t;
}

// ===================================================================
// project_tree tool
// ===================================================================

static Tool make_project_tree_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    CancellationToken cancelled = nullptr) {
    Tool t;
    t.name = "project_tree";
    t.description =
        "Recursively list files/directories in a tree-like format.\n"
        "Maximum depth of 5 to avoid huge outputs. "
        "Use this to understand project structure in a single call "
        "instead of calling list_files repeatedly.";
    t.timeout_sec = 5;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description",
                        "Starting directory path (default '.')"}}},
                {"max_depth",
                    {{"type", "integer"},
                        {"description",
                            "Maximum recursion depth (default 5, max 10)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum output lines (default 500, max 500)"}}}}}};
    t.execute = [safe_dir_ptr, read_only_paths, cancelled](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string("."));
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int max_depth = args.value("max_depth", 5);
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 10) max_depth = 10;

        int max_lines = args.value("max_lines", 500);
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

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

// ===================================================================


// ===================================================================
// delete_file
// ===================================================================

static Tool make_delete_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "delete_file";
    t.description = "Delete a file";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path to delete"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (!std::filesystem::exists(*resolved, ec)) {
            return std::unexpected("File not found: " + *resolved);
        }
        if (!std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }
        uintmax_t size = std::filesystem::file_size(*resolved, ec);
        bool removed = std::filesystem::remove(*resolved, ec);
        if (ec || !removed) {
            return std::unexpected("Failed to delete file: " + ec.message());
        }
        return "ok (deleted " + *resolved + ", " + std::to_string(size) + " bytes)";
    };
    return t;
}

// ===================================================================
// move_file
// ===================================================================

static Tool make_move_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "move_file";
    t.description = "Move or rename a file from source to destination. "
                    "Works for both same-directory renames and cross-directory moves. "
                    "Will not overwrite an existing destination.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"source", {{"type", "string"}, {"description", "Source file path"}}},
             {"destination", {{"type", "string"}, {"description", "Destination file path"}}}}},
        {"required", {"source", "destination"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto src_raw = args.value("source", std::string());
        auto dst_raw = args.value("destination", std::string());

        if (src_raw.empty()) {
            return std::unexpected(std::string("source is required"));
        }
        if (dst_raw.empty()) {
            return std::unexpected(std::string("destination is required"));
        }

        auto src = resolve_path(src_raw, *safe_dir_ptr);
        if (!src) return std::unexpected(src.error());
        auto dst = resolve_path(dst_raw, *safe_dir_ptr);
        if (!dst) return std::unexpected(dst.error());

        std::error_code ec;
        if (!std::filesystem::exists(*src, ec)) {
            return std::unexpected("Source not found: " + *src);
        }
        if (!std::filesystem::is_regular_file(*src, ec)) {
            return std::unexpected("Source is not a regular file: " + *src);
        }
        if (std::filesystem::exists(*dst, ec)) {
            return std::unexpected("Destination already exists: " + *dst);
        }

        // Create parent directories of destination
        std::filesystem::create_directories(std::filesystem::path(*dst).parent_path(), ec);
        if (ec) {
            return std::unexpected(
                "Failed to create parent directories: " + ec.message());
        }

        std::filesystem::rename(*src, *dst, ec);
        if (ec) {
            // Cross-device link: fall back to copy + delete
            if (ec.value() == EXDEV) {
                std::filesystem::copy_file(*src, *dst, std::filesystem::copy_options::none, ec);
                if (ec) {
                    return std::unexpected(
                        "Failed to copy across devices: " + ec.message());
                }
                std::filesystem::remove(*src, ec);
                if (ec) {
                    // Clean up destination
                    std::filesystem::remove(*dst, ec);
                    return std::unexpected(
                        "Failed to remove source after copy: " + ec.message());
                }
            } else {
                return std::unexpected("Failed to move file: " + ec.message());
            }
        }

        return "ok (moved " + *src + " \u2192 " + *dst + ")";
    };
    return t;
}

// ===================================================================
// rename_file
// ===================================================================

static Tool make_rename_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "rename_file";
    t.description =
        "Rename a file within its directory. "
        "Provide the current path and the new filename (basename only, no path separators). "
        "For cross-directory moves, use move_file instead.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Current file path"}}},
             {"new_name",
                 {{"type", "string"},
                     {"description",
                         "New filename (basename only, no '/' or '\\' allowed)"}}}}},
        {"required", {"path", "new_name"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw_path = args.value("path", std::string());
        auto new_name = args.value("new_name", std::string());

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (new_name.empty()) {
            return std::unexpected(std::string("new_name is required"));
        }
        if (new_name.find('/') != std::string::npos ||
            new_name.find('\\') != std::string::npos) {
            return std::unexpected(
                "new_name must be a filename, not a path "
                "(no '/' or '\\' allowed)");
        }

        auto resolved = resolve_path(raw_path, *safe_dir_ptr);
        if (!resolved) return std::unexpected(resolved.error());

        std::error_code ec;
        if (!std::filesystem::exists(*resolved, ec)) {
            return std::unexpected("File not found: " + *resolved);
        }
        if (!std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        auto parent = std::filesystem::path(*resolved).parent_path();
        auto destination = (parent / new_name).string();

        // Resolve destination to ensure it's within safe_dir
        auto dst_resolved = resolve_path(destination, *safe_dir_ptr);
        if (!dst_resolved) return std::unexpected(dst_resolved.error());

        if (std::filesystem::exists(*dst_resolved, ec)) {
            return std::unexpected(
                "Destination already exists: " + *dst_resolved);
        }

        std::filesystem::rename(*resolved, *dst_resolved, ec);
        if (ec) {
            return std::unexpected("Failed to rename file: " + ec.message());
        }

        return "ok (renamed " + *resolved + " \u2192 " + *dst_resolved + ")";
    };
    return t;
}

// ===================================================================
// Safe recursive directory deletion (never follows symlinks)
// ===================================================================

/// Recursively delete \p dir without following any symlinks.
/// Symlinks are removed with std::filesystem::remove() which unlinks only
/// the symlink itself, never the target. Real directories are recursed into.
static void remove_all_safe(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return;
    for (const auto& entry : it) {
        if (entry.is_symlink()) {
            // remove() on a symlink unlinks only the symlink itself — safe.
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        } else if (entry.is_directory(ec)) {
            ec.clear();
            remove_all_safe(entry.path());
        } else {
            std::filesystem::remove(entry.path(), ec);
            ec.clear();
        }
    }
    std::filesystem::remove(dir, ec);
}

// ===================================================================
// Worktree state — shared mutable data between start_worktree and
// stop_worktree for a single agent session.
// ===================================================================

struct WorktreeState {
    std::string original_safe_dir;   // main repo path (to restore on stop)
    std::string worktree_name;       // git worktree name
    std::string worktree_path;       // filesystem path to the worktree
    std::string branch_name;         // branch name checked out in the worktree
    bool active = false;
};

// Forward-declared in tools.h as struct WorktreeState.

/// Sanitize a branch name for use as a filesystem directory component.
/// Replaces '/' and other problematic characters with '-'.
static std::string sanitize_branch_name(const std::string& branch) {
    std::string out;
    out.reserve(branch.size());
    for (char c : branch) {
        if (c == '/' || c == '\\' || c == '\0' || c == '.' || c == ' ') {
            out += '-';
        } else {
            out += c;
        }
    }
    return out;
}

// ===================================================================
// start_worktree tool
// ===================================================================

Tool make_start_worktree_tool(std::shared_ptr<std::string> safe_dir_ptr,
    std::shared_ptr<std::string> worktree_base_ptr,
    std::shared_ptr<WorktreeState> state) {

    Tool t;
    t.name = "start_worktree";
    t.description =
        "Create a git worktree at a temporary location and set the agent's "
        "working directory to it. All subsequent file/git/bash tools operate "
        "within this worktree until stop_worktree is called. "
        "Multiple agents can each have their own active worktree in parallel. "
        "Each agent must use a unique branch name — if the branch is already "
        "checked out in another worktree, the tool will fail with a clear error. "
        "Uses libgit2 directly (no git CLI).";
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"branch",
                {{"type", "string"},
                    {"description",
                        "Branch name to create and check out in the worktree. "
                        "If the branch doesn't exist, it is created from HEAD."}}}}},
        {"required", {"branch"}}};

    t.execute = [safe_dir_ptr, worktree_base_ptr, state](const json& args) -> Result<std::string> {
        if (state->active) {
            return std::unexpected("A worktree is already active: " + state->worktree_path +
                ". Call stop_worktree first.");
        }

        auto branch = args.value("branch", std::string());
        if (branch.empty()) {
            return std::unexpected("branch is required");
        }

        // Determine worktree path (always under worktree_base)
        pid_t pid = getpid();
        std::string sanitized = sanitize_branch_name(branch);
        std::string wt_name = std::to_string(pid) + "-" + sanitized;
        std::string wt_path = *worktree_base_ptr + "/" + wt_name;

        // Ensure the base directory exists
        std::error_code ec;
        std::filesystem::create_directories(*worktree_base_ptr, ec);
        if (ec) {
            return std::unexpected("start_worktree: cannot create base directory '" +
                *worktree_base_ptr + "': " + ec.message());
        }

        // Open the main repo
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        // Look up or create the branch reference
        git_reference* branch_ref = nullptr;
        int err = git_branch_lookup(&branch_ref, repo, branch.c_str(), GIT_BRANCH_LOCAL);
        if (err == GIT_ENOTFOUND) {
            // Branch doesn't exist — create it from HEAD
            git_commit* head_commit = nullptr;
            err = git_revparse_single(reinterpret_cast<git_object**>(&head_commit),
                repo, "HEAD^{commit}");
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("start_worktree: cannot resolve HEAD: " +
                    (e ? std::string(e->message) : "unknown error"));
            }
            auto commit_cleanup = std::unique_ptr<git_commit, decltype(&git_commit_free)>(
                head_commit, git_commit_free);

            err = git_branch_create(&branch_ref, repo, branch.c_str(), head_commit, 0);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("start_worktree: cannot create branch '" + branch + "': " +
                    (e ? std::string(e->message) : "unknown error"));
            }
        } else if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("start_worktree: error looking up branch '" + branch + "': " +
                (e ? std::string(e->message) : "unknown error"));
        }
        auto branch_cleanup = std::unique_ptr<git_reference, decltype(&git_reference_free)>(
            branch_ref, git_reference_free);

        // Create the worktree
        git_worktree* wt = nullptr;
        git_worktree_add_options wt_opts = GIT_WORKTREE_ADD_OPTIONS_INIT;
        wt_opts.ref = branch_ref;
        wt_opts.lock = 1;            // lock immediately to prevent external pruning

        err = git_worktree_add(&wt, repo, wt_name.c_str(), wt_path.c_str(), &wt_opts);
        if (err) {
            const git_error* e = git_error_last();
            std::string msg = e ? e->message : "unknown error";
            // Detect "branch already checked out elsewhere" — common cross-agent issue
            if (msg.find("already checked out") != std::string::npos ||
                msg.find("already exists") != std::string::npos) {
                msg += "\n\nThe branch '" + branch +
                    "' is already checked out in another worktree. "
                    "Each agent needs its own branch name. "
                    "Use a different branch name and try again.";
            }
            return std::unexpected("start_worktree: git_worktree_add failed: " + msg);
        }
        auto wt_cleanup = std::unique_ptr<git_worktree, decltype(&git_worktree_free)>(
            wt, git_worktree_free);

        // Store state
        state->worktree_name = wt_name;
        state->worktree_path = wt_path;
        state->branch_name = branch;
        state->active = true;

        // Switch the session's safe_dir to the worktree path
        *safe_dir_ptr = wt_path;

        return "Worktree created at " + wt_path + " on branch '" + branch +
            "'. All tools now operate within this worktree. "
            "Call stop_worktree to return to the main repository.";
    };
    return t;
}

// ===================================================================
// Check for uncommitted changes in the worktree
// Returns a non-empty string describing changes if any exist, or empty
// if the worktree is clean.
// ===================================================================

/// Check whether there are uncommitted changes in the worktree at \p repo_dir.
/// Returns a human-readable summary of changes, or an empty string if clean.
static Result<std::string> check_worktree_dirty(git_repository* repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
               | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
               | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY
               | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;

    std::vector<std::string> entries;
    const int max_entries = 50;

    struct CbPayload {
        std::vector<std::string>* out;
        int max;
    };
    CbPayload payload{&entries, max_entries};

    auto cb = [](const char* path, unsigned int status_flags, void* data) -> int {
        auto* p = static_cast<CbPayload*>(data);
        if (static_cast<int>(p->out->size()) >= p->max)
            return 1; // abort iteration

        // Skip ignored files
        if (status_flags & GIT_STATUS_IGNORED)
            return 0;

        // Only report actual non-clean entries
        if (status_flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                            GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                            GIT_STATUS_INDEX_TYPECHANGE |
                            GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
                            GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED |
                            GIT_STATUS_WT_TYPECHANGE |
                            GIT_STATUS_CONFLICTED)) {
            p->out->push_back(path ? path : "(unknown)");
        }
        return 0;
    };

    int err = git_status_foreach_ext(repo, &opts, cb, &payload);
    if (err < 0 && err != GIT_EUSER) {
        const git_error* e = git_error_last();
        return std::unexpected("failed to check worktree status: " +
            (e ? std::string(e->message) : "unknown error"));
    }

    if (entries.empty())
        return std::string(); // clean

    // Build summary
    std::string summary = std::to_string(entries.size()) + " uncommitted change(s):\n";
    for (const auto& e : entries) {
        summary += "  - " + e + "\n";
    }
    if (entries.size() >= static_cast<size_t>(max_entries)) {
        summary += "  ...(truncated)\n";
    }
    return summary;
}

// ===================================================================
// Check whether a branch is fully merged into HEAD.
// Returns true if the branch tip is an ancestor of HEAD (all commits
// on the branch are reachable from HEAD).
// ===================================================================

static Result<bool> is_branch_merged(git_repository* repo, const std::string& branch) {
    // Look up the branch
    git_reference* branch_ref = nullptr;
    int err = git_branch_lookup(&branch_ref, repo, branch.c_str(), GIT_BRANCH_LOCAL);
    if (err) {
        // Branch doesn't exist — treat as merged (nothing to lose)
        if (err == GIT_ENOTFOUND)
            return true;
        const git_error* e = git_error_last();
        return std::unexpected("cannot look up branch '" + branch + "': " +
            (e ? std::string(e->message) : "unknown error"));
    }
    auto branch_cleanup = std::unique_ptr<git_reference, decltype(&git_reference_free)>(
        branch_ref, git_reference_free);

    // Get branch tip commit
    git_commit* branch_commit = nullptr;
    err = git_commit_lookup(&branch_commit, repo, git_reference_target(branch_ref));
    if (err) {
        const git_error* e = git_error_last();
        return std::unexpected("cannot look up branch tip: " +
            (e ? std::string(e->message) : "unknown error"));
    }
    auto branch_commit_cleanup = std::unique_ptr<git_commit, decltype(&git_commit_free)>(
        branch_commit, git_commit_free);

    // Get HEAD commit
    git_commit* head_commit = nullptr;
    err = git_revparse_single(reinterpret_cast<git_object**>(&head_commit),
        repo, "HEAD^{commit}");
    if (err) {
        const git_error* e = git_error_last();
        return std::unexpected("cannot resolve HEAD: " +
            (e ? std::string(e->message) : "unknown error"));
    }
    auto head_commit_cleanup = std::unique_ptr<git_commit, decltype(&git_commit_free)>(
        head_commit, git_commit_free);

    // Find merge base
    git_oid merge_base_oid;
    err = git_merge_base(&merge_base_oid, repo,
        git_commit_id(head_commit), git_commit_id(branch_commit));
    if (err) {
        const git_error* e = git_error_last();
        return std::unexpected("cannot compute merge base: " +
            (e ? std::string(e->message) : "unknown error"));
    }

    // Branch is fully merged into HEAD iff the merge base is the branch tip
    // (i.e. every commit on the branch is also reachable from HEAD).
    const git_oid* branch_oid = git_commit_id(branch_commit);
    return git_oid_equal(&merge_base_oid, branch_oid);
}

// ===================================================================
// stop_worktree tool
// ===================================================================

Tool make_stop_worktree_tool(std::shared_ptr<std::string> safe_dir_ptr,
    std::shared_ptr<WorktreeState> state) {

    Tool t;
    t.name = "stop_worktree";
    t.description =
        "Stop the current worktree session and return to the main repository. "
        "Cleans up the worktree directory, git worktree metadata, "
        "and deletes the worktree branch. "
        "After calling this, all tools operate on the original repository again.\n"
        "Requires `force: true` if the worktree has uncommitted changes, "
        "or if the branch has commits not yet merged into HEAD.\n"
        "Use `force: true` to discard uncommitted changes and delete the branch.";
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"force",
                {{"type", "boolean"},
                    {"description",
                        "If true, discard uncommitted changes and delete the branch "
                        "even if it is dirty or unmerged. Default false."}}}}},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr, state](const json& args) -> Result<std::string> {
        if (!state->active) {
            return std::unexpected("No worktree is currently active. "
                "Call start_worktree first.");
        }

        bool force = args.value("force", false);

        // Open the main repo (safe_dir_ptr still points to worktree path,
        // but git_repository_open_ext follows .git files so it finds the main repo)
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        // Check for uncommitted changes (unless forced)
        if (!force) {
            auto dirty = check_worktree_dirty(repo);
            if (!dirty) {
                return std::unexpected("stop_worktree: " + dirty.error());
            }
            if (!dirty->empty()) {
                return std::unexpected(
                    "stop_worktree: worktree has uncommitted changes.\n" +
                    *dirty +
                    "Use stop_worktree with {\"force\": true} to discard them and proceed.");
            }
        }

        // Check if the branch is merged into the main repo's HEAD (unless forced).
        // NOTE: we open a SEPARATE repo from the original safe dir here, because
        // the worktree repo has HEAD pointing to the worktree branch itself
        // (e.g. "testing"), so the merge check would always succeed.
        // Opening from original_safe_dir gives the main repository HEAD (e.g. "master").
        if (!force && !state->branch_name.empty()) {
            auto main_repo_res = open_git_repo(state->original_safe_dir);
            if (!main_repo_res) {
                return std::unexpected("stop_worktree: " + main_repo_res.error());
            }
            git_repository* main_repo = *main_repo_res;
            auto main_repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
                main_repo, git_repository_free);

            auto merged = is_branch_merged(main_repo, state->branch_name);
            if (!merged) {
                return std::unexpected("stop_worktree: " + merged.error());
            }
            if (!*merged) {
                return std::unexpected(
                    "stop_worktree: branch '" + state->branch_name +
                    "' has commits not yet merged into HEAD.\n"
                    "Use stop_worktree with {\"force\": true} to delete the branch anyway.");
            }
        }

        // Look up the worktree (may have been removed by another agent or manually)
        git_worktree* wt = nullptr;
        int err = git_worktree_lookup(&wt, repo, state->worktree_name.c_str());
        if (err == 0 && wt) {
            auto wt_cleanup = std::unique_ptr<git_worktree, decltype(&git_worktree_free)>(
                wt, git_worktree_free);

            // Unlock the worktree (required before prune)
            err = git_worktree_unlock(wt);
            if (err && err != GIT_ELOCKED) {
                const git_error* e = git_error_last();
                return std::unexpected("stop_worktree: unlock failed: " +
                    (e ? std::string(e->message) : "unknown error"));
            }

            // Prune the git worktree metadata (.git/worktrees/<name>)
            git_worktree_prune_options prune_opts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
            prune_opts.flags = GIT_WORKTREE_PRUNE_VALID;
            err = git_worktree_prune(wt, &prune_opts);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("stop_worktree: prune failed: " +
                    (e ? std::string(e->message) : "unknown error"));
            }
        }

        // Safely delete the worktree filesystem directory (never follows symlinks)
        // Even if git metadata was already removed, clean up the filesystem.
        std::error_code ec;
        if (std::filesystem::exists(state->worktree_path, ec)) {
            remove_all_safe(state->worktree_path);
        }

        // Delete the worktree branch
        if (!state->branch_name.empty()) {
            git_reference* branch_ref = nullptr;
            int err = git_branch_lookup(&branch_ref, repo,
                state->branch_name.c_str(), GIT_BRANCH_LOCAL);
            if (err == 0 && branch_ref) {
                auto branch_cleanup = std::unique_ptr<git_reference,
                    decltype(&git_reference_free)>(branch_ref, git_reference_free);
                err = git_branch_delete(branch_ref);
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("stop_worktree: failed to delete branch '" +
                        state->branch_name + "': " +
                        (e ? std::string(e->message) : "unknown error"));
                }
            }
        }

        // Restore safe_dir to original repo
        *safe_dir_ptr = state->original_safe_dir;

        // Clear state
        std::string wt_name = state->worktree_name;
        std::string wt_path = state->worktree_path;
        std::string branch_name = state->branch_name;
        state->worktree_name.clear();
        state->worktree_path.clear();
        state->branch_name.clear();
        state->active = false;

        return "Worktree '" + wt_name + "' (branch '" + branch_name +
            "') has been cleaned up. Tools now operate on the main repository.";
    };
    return t;
}

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

void ToolRegistry::add_defaults(const std::string& safe_dir,
    const std::vector<std::string>& read_only_paths,
    const std::string& search_api_key,
    const std::string& search_engine_id,
    const std::string& search_endpoint,
    const std::string& worktree_base,
    bool include_write) {
    add_defaults(std::make_shared<std::string>(safe_dir),
        read_only_paths, search_api_key, search_engine_id,
        search_endpoint, worktree_base, include_write);
}

void ToolRegistry::add_defaults(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    const std::string& search_api_key,
    const std::string& search_engine_id,
    const std::string& search_endpoint,
    const std::string& worktree_base,
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

    // ── Worktree tools (always included, Internal permission) ──
    // Both tools share a single WorktreeState so stop_worktree can see
    // what start_worktree recorded.
    auto wt_state = std::make_shared<WorktreeState>();
    wt_state->original_safe_dir = *safe_dir_ptr;
    {
        auto t = make_start_worktree_tool(safe_dir_ptr,
            std::make_shared<std::string>(worktree_base),
            wt_state);
        t.permission = ToolPermission::Internal;
        add(std::move(t));
    }
    {
        auto t = make_stop_worktree_tool(safe_dir_ptr, wt_state);
        t.permission = ToolPermission::Internal;
        add(std::move(t));
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
