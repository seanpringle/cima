#include "tools.h"

#include <git2.h>
#include <mutex>
#include <string>
#include <vector>

// Global mutex to serialize git index write operations (git_add, git_commit, git_restore).
// libgit2 acquires a file lock on .git/index.lock when opening the index;
// concurrent index writes from parallel tool calls cause GIT_ELOCKED errors.
static std::mutex g_git_index_mutex;

Tool make_git_status_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout) {
    Tool t;
    t.name = "git_status";
    t.description =
        "Return the working tree status in short format (like 'git status --short').\n"
        "Each line uses the two-character porcelain format:\n"
        "  XY <path>\n"
        "where X is the index status and Y is the working tree status.\n"
        "  ' ' = unmodified, M = modified, A = added, D = deleted, "
        "R = renamed, C = copied, U = updated, ? = untracked, ! = ignored\n"
        "Output is sorted by path.";
    t.timeout_sec = timeout;
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

        git_status_options opts = GIT_STATUS_OPTIONS_INIT;
        opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED
                   | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS
                   | GIT_STATUS_OPT_INCLUDE_IGNORED
                   | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

        auto cb = [](const char* path, unsigned int status_flags, void* data) -> int {
            auto* out = static_cast<std::vector<Entry>*>(data);
            Entry e;
            e.path = path ? path : "";
            e.x = status_char_for_index(status_flags);
            e.y = status_char_for_workdir(status_flags);
            out->push_back(std::move(e));
            return 0; // continue
        };

        int err = git_status_foreach_ext(repo, &opts, cb, &entries);
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
        for (const auto& e : entries) {
            result += e.x;
            result += e.y;
            result += ' ';
            result += e.path;
            result += '\n';
        }
        if (result.empty()) {
            result = "(clean — no changes)";
        }
        return result;
    };
    return t;
}

Tool make_git_diff_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "git_diff";
    t.description =
        "Return a unified diff of unstaged (or staged) changes.\n"
        "Use git_status first to see which files have changed, "
        "then git_diff to inspect the actual changes.\n"
        "If 'staged' is true, shows the diff that would be committed "
        "(index vs HEAD). If false (default), shows unstaged changes "
        "(working tree vs index).";
    t.timeout_sec = timeout;
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

    t.execute = [safe_dir_ptr, tool_logs](const json& args) -> Result<std::string> {
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

        // Print unified diff into a string
        std::string result;

        auto print_cb = [](const git_diff_delta* /*delta*/,
                           const git_diff_hunk* /*hunk*/,
                           const git_diff_line* line,
                           void* payload) -> int {
            auto* output = static_cast<std::string*>(payload);
            // Prepend origin character for +/-/context lines
            if (line->origin == '+' || line->origin == '-' || line->origin == ' ') {
                output->push_back(line->origin);
            }
            output->append(line->content, line->content_len);
            return 0;
        };

        err = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, print_cb, &result);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_diff print error: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        if (result.empty()) {
            result = staged ? "(no staged changes)" : "(no unstaged changes)";
        }

        return spill_long_output(std::move(result), tool_logs);
    };
    return t;
}

Tool make_git_log_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
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
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"max_count",
                {{"type", "integer"},
                    {"description", "Maximum number of commits to return (default 10)"}}},
             {"format",
                {{"type", "string"},
                    {"enum", {"oneline", "short", "full"}},
                    {"description", "Output format: oneline, short (default), or full"}}},
             {"branch",
                {{"type", "string"},
                    {"description", "Git revision to start from (e.g. 'main', 'HEAD~3', 'v1.0'). "
                                   "Defaults to HEAD."}}}}},
        {"required", json::array()}};

    t.execute = [safe_dir_ptr, tool_logs](const json& args) -> Result<std::string> {
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

        git_oid oid;
        while (git_revwalk_next(&oid, walk) == 0 && count < max_count) {

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

        return spill_long_output(std::move(result), tool_logs);
    };
    return t;
}

Tool make_git_add_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout) {
    Tool t;
    t.name = "git_add";
    t.description =
        "Stage file(s) for commit. Like 'git add <path>' or 'git add -A'.\n"
        "If 'all' is true, stages all changes (added, modified, deleted) "
        "in the entire working tree.\n"
        "If 'path' is specified, only that file or pathspec is staged.\n"
        "Use git_status first to see which files are changed, "
        "then git_add to stage them, then git_commit to commit.";
    t.timeout_sec = timeout;
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

Tool make_git_commit_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout) {
    Tool t;
    t.name = "git_commit";
    t.description =
        "Create a new commit with staged changes. Like 'git commit -m <message>'.\n"
        "If 'all' is true, stages all changes before committing (like 'git commit -a').\n"
        "Use git_status first to see which files are changed, "
        "then git_add to stage them, then git_commit to commit.";
    t.timeout_sec = timeout;
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
// git_restore
// ===================================================================

Tool make_git_restore_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout) {
    Tool t;
    t.name = "git_restore";
    t.description =
        "Restore working tree files or unstage changes. "
        "Like 'git restore <path>' or 'git restore --staged <path>'.\n"
        "If 'staged' is false (default), discards unstaged changes "
        "in the working tree (files are restored from HEAD).\n"
        "If 'staged' is true, restores the index entry to match "
        "HEAD (or source), keeping working tree changes.\n"
        "Use 'source' to restore from a specific revision instead of "
        "HEAD, e.g. source=\"HEAD~1\", source=\"main\".\n"
        "Use path=\".\" to restore all files.\n"
        "This tool modifies files on disk — use with care.";
    t.permission = ToolPermission::Write;
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description",
                        "File path to restore. "
                        "Use \".\" to restore all files in the working tree. "
                        "Must be under the safe directory."}}},
             {"staged",
                {{"type", "boolean"},
                    {"description",
                        "If true, restore index entry to match HEAD (or "
                        "source), keeping working tree changes. "
                        "If false (default), discard working tree changes."}}},
             {"source",
                {{"type", "string"},
                    {"description",
                        "Optional revision to restore from "
                        "(e.g. \"HEAD~1\", \"main\", commit hash). "
                        "Defaults to HEAD."}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        // Serialize with other git index write operations (for staged mode)
        // Unstaged restoration doesn't touch the index, but we lock anyway
        // since restoring files modifies the working tree.
        std::lock_guard<std::mutex> lock(g_git_index_mutex);

        // Open git repository
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        std::string raw_path = args.value("path", std::string());
        if (raw_path.empty()) {
            return std::unexpected("path is required");
        }

        bool staged = args.value("staged", false);
        std::string source = args.value("source", std::string());

        // Resolve the path if it's not "." (which means "all files")
        std::string resolved_path;
        if (raw_path != ".") {
            auto resolved = resolve_path(raw_path, *safe_dir_ptr);
            if (!resolved) {
                return std::unexpected(resolved.error());
            }
            resolved_path = *resolved;
        }

        // Build pathspec for checkout/remove
        // libgit2 pathspecs are relative to the workdir root
        const char* workdir = git_repository_workdir(repo);
        if (!workdir) {
            return std::unexpected("repository has no working directory");
        }
        std::string workdir_str(workdir);
        if (!workdir_str.empty() && workdir_str.back() != '/')
            workdir_str += '/';

        std::string rel_path;
        if (raw_path == ".") {
            rel_path = ".";
        } else {
            // Convert resolved absolute path to relative for libgit2
            if (resolved_path.size() > workdir_str.size() &&
                resolved_path.compare(0, workdir_str.size(), workdir_str) == 0) {
                rel_path = resolved_path.substr(workdir_str.size());
            } else {
                return std::unexpected("path is outside the repository working directory");
            }
        }

        if (staged) {
            // ── Unstage mode: restore index entries to match HEAD (or source) ──
            git_index* index = nullptr;
            int err = git_repository_index(&index, repo);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_restore error opening index: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
            auto index_cleanup = std::unique_ptr<git_index, decltype(&git_index_free)>(
                index, git_index_free);

            // Resolve the source tree (HEAD or specified revision)
            git_object* target_obj = nullptr;
            if (!source.empty()) {
                err = git_revparse_single(&target_obj, repo, source.c_str());
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_restore error resolving source '" +
                        source + "': " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                }
            } else {
                err = git_revparse_single(&target_obj, repo, "HEAD^{tree}");
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_restore error resolving HEAD: " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                }
            }
            auto obj_cleanup = std::unique_ptr<git_object, decltype(&git_object_free)>(
                target_obj, git_object_free);

            // Peel to a tree
            git_tree* source_tree = nullptr;
            if (target_obj) {
                if (git_object_type(target_obj) == GIT_OBJECT_TREE) {
                    source_tree = reinterpret_cast<git_tree*>(target_obj);
                    // Transfer ownership: release obj_cleanup since
                    // source_tree (via tree_cleanup) now owns the ref
                    obj_cleanup.release();
                } else {
                    err = git_commit_tree(&source_tree,
                        reinterpret_cast<git_commit*>(target_obj));
                    if (err) {
                        const git_error* e = git_error_last();
                        return std::unexpected("git_restore error getting source tree: " +
                            (e ? std::string(e->message) : std::string("unknown error")));
                    }
                }
            }
            auto tree_cleanup = std::unique_ptr<git_tree, decltype(&git_tree_free)>(
                source_tree, git_tree_free);

            if (raw_path == ".") {
                // Restore all index entries from source tree
                err = git_index_read_tree(index, source_tree);
                if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_restore error restoring index: " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                }
            } else {
                // Look up the path in the source tree
                git_tree_entry* tree_entry = nullptr;
                err = git_tree_entry_bypath(&tree_entry, source_tree, rel_path.c_str());
                if (err == GIT_ENOTFOUND) {
                    // File doesn't exist in source — remove from index
                    err = git_index_remove_bypath(index, rel_path.c_str());
                    if (err && err != GIT_ENOTFOUND) {
                        const git_error* e = git_error_last();
                        return std::unexpected("git_restore error removing '" +
                            raw_path + "': " +
                            (e ? std::string(e->message) : std::string("unknown error")));
                    }
                } else if (err) {
                    const git_error* e = git_error_last();
                    return std::unexpected("git_restore error looking up '" +
                        raw_path + "' in source tree: " +
                        (e ? std::string(e->message) : std::string("unknown error")));
                } else {
                    // Restore index entry from the tree entry
                    auto entry_cleanup = std::unique_ptr<git_tree_entry,
                        decltype(&git_tree_entry_free)>(
                        tree_entry, git_tree_entry_free);

                    const git_oid* oid = git_tree_entry_id(tree_entry);
                    git_filemode_t mode = git_tree_entry_filemode(tree_entry);

                    // Add the tree entry back to the index with its original OID and mode
                    git_index_entry ie;
                    memset(&ie, 0, sizeof(ie));
                    ie.mode = mode;
                    ie.path = rel_path.c_str();
                    git_oid_cpy(&ie.id, oid);

                    err = git_index_add(index, &ie);
                    if (err) {
                        const git_error* e = git_error_last();
                        return std::unexpected("git_restore error adding to index: " +
                            (e ? std::string(e->message) : std::string("unknown error")));
                    }
                }
            }

            err = git_index_write(index);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_restore error writing index: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }

            std::string msg = "Unstaged";
            if (!source.empty()) {
                msg += " from '" + source + "'";
            }
            if (raw_path == ".") {
                msg += " all files";
            } else {
                msg += " '" + raw_path + "'";
            }
            return msg;
        }

        // ── Discard mode: checkout from source (or HEAD) ──
        git_object* target_obj = nullptr;
        if (!source.empty()) {
            int err = git_revparse_single(&target_obj, repo, source.c_str());
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_restore error resolving source '" +
                    source + "': " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        }

        auto obj_cleanup = std::unique_ptr<git_object, decltype(&git_object_free)>(
            target_obj, git_object_free);

        // Set up checkout options with pathspec
        git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
        checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;

        const char* paths_arr[1];
        if (raw_path != ".") {
            paths_arr[0] = rel_path.c_str();
            checkout_opts.paths.strings = const_cast<char**>(paths_arr);
            checkout_opts.paths.count = 1;
        }

        if (target_obj) {
            // Checkout from specific tree/commit
            int err = git_checkout_tree(repo, target_obj, &checkout_opts);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_restore error restoring from '" +
                    source + "': " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        } else {
            // Checkout from HEAD (default)
            int err = git_checkout_head(repo, &checkout_opts);
            if (err) {
                const git_error* e = git_error_last();
                return std::unexpected("git_restore error: " +
                    (e ? std::string(e->message) : std::string("unknown error")));
            }
        }

        std::string msg = "Restored";
        if (!source.empty()) {
            msg += " from '" + source + "'";
        }
        if (raw_path == ".") {
            msg += " all files";
        } else {
            msg += " '" + raw_path + "'";
        }
        return msg;
    };
    return t;
}

// ===================================================================
// git_show
// ===================================================================

Tool make_git_show_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "git_show";
    t.description =
        "Show a commit's full diff and metadata. "
        "Like 'git show <revision>'.\n"
        "Shows the commit hash, author, date, full message, "
        "and unified diff of changes introduced by that commit.\n"
        "Use 'revision' to specify a commit "
        "(hash, branch name, HEAD~N, tag, etc.).\n"
        "Defaults to HEAD.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"revision",
                {{"type", "string"},
                    {"description",
                        "Git revision to show (commit hash, branch, "
                        "HEAD~N, tag, etc.). Defaults to HEAD."}}}}},
        {"required", json::array()}};
    t.execute = [safe_dir_ptr, tool_logs](const json& args) -> Result<std::string> {
        // Open repo
        auto repo_res = open_git_repo(*safe_dir_ptr);
        if (!repo_res) {
            return std::unexpected(repo_res.error());
        }
        git_repository* repo = *repo_res;
        auto repo_cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(
            repo, git_repository_free);

        std::string revision = args.value("revision", std::string("HEAD"));

        // Resolve revision to a commit object
        git_object* obj = nullptr;
        int err = git_revparse_single(&obj, repo, revision.c_str());
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_show error resolving '" + revision + "': " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        auto obj_cleanup = std::unique_ptr<git_object, decltype(&git_object_free)>(
            obj, git_object_free);

        if (git_object_type(obj) != GIT_OBJECT_COMMIT) {
            return std::unexpected("git_show error: '" + revision + "' is not a commit");
        }

        git_commit* commit = reinterpret_cast<git_commit*>(obj);
        // obj_cleanup will free it — we just borrow the reference

        // Build commit metadata
        char hex[GIT_OID_HEXSZ + 1];
        git_oid_tostr(hex, sizeof(hex), git_commit_id(commit));

        // Author info
        const git_signature* author = git_commit_author(commit);
        std::string author_str = "unknown <unknown>";
        if (author) {
            author_str = (author->name ? author->name : "unknown");
            author_str += " <";
            author_str += (author->email ? author->email : "unknown");
            author_str += ">";
        }

        // Date
        time_t commit_time = git_commit_time(commit);
        int time_offset = git_commit_time_offset(commit);
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

        // Message
        const char* raw_msg = git_commit_message(commit);
        std::string msg(raw_msg ? raw_msg : "");

        // ── Diff ──
        // Get the commit's tree
        git_tree* commit_tree = nullptr;
        err = git_commit_tree(&commit_tree, commit);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_show error getting commit tree: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        auto tree_cleanup = std::unique_ptr<git_tree, decltype(&git_tree_free)>(
            commit_tree, git_tree_free);

        // Get parent tree (null tree for root commit)
        git_tree* parent_tree = nullptr;
        auto parent_cleanup = std::unique_ptr<git_tree, decltype(&git_tree_free)>(
            nullptr, git_tree_free);

        if (git_commit_parentcount(commit) > 0) {
            git_commit* parent = nullptr;
            err = git_commit_parent(&parent, commit, 0);
            if (!err) {
                auto parent_commit_cleanup = std::unique_ptr<git_commit, decltype(&git_commit_free)>(
                    parent, git_commit_free);
                err = git_commit_tree(&parent_tree, parent);
                if (err) {
                    parent_tree = nullptr;
                }
                parent_cleanup.reset(parent_tree);
            }
        }

        // Diff commit vs parent
        git_diff* diff = nullptr;
        git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
        // Show context of 3 lines
        diff_opts.context_lines = 3;

        err = git_diff_tree_to_tree(&diff, repo, parent_tree, commit_tree, &diff_opts);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_show error creating diff: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }
        auto diff_cleanup = std::unique_ptr<git_diff, decltype(&git_diff_free)>(
            diff, git_diff_free);

        // Build metadata header first
        std::string result;
        result += "commit " + std::string(hex) + "\n";
        result += "Author: " + author_str + "\n";
        result += "Date:   " + date_str + "\n\n";

        // Indent message
        if (!msg.empty()) {
            std::string full_msg = msg;
            while (!full_msg.empty() &&
                   (full_msg.back() == '\n' || full_msg.back() == '\r'))
                full_msg.pop_back();
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

        // Count files changed
        size_t num_deltas = git_diff_num_deltas(diff);
        if (num_deltas > 0) {
            result += std::to_string(num_deltas) +
                (num_deltas == 1 ? " file changed" : " files changed") + ":\n";
        } else {
            result += "(no diff — root commit or empty commit)\n";
            return result;
        }

        // Print diff
        auto print_cb = [](const git_diff_delta* /*delta*/,
                           const git_diff_hunk* /*hunk*/,
                           const git_diff_line* line,
                           void* payload) -> int {
            auto* output = static_cast<std::string*>(payload);
            if (line->origin == '+' || line->origin == '-' || line->origin == ' ') {
                output->push_back(line->origin);
            }
            output->append(line->content, line->content_len);
            return 0;
        };

        err = git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, print_cb, &result);
        if (err) {
            const git_error* e = git_error_last();
            return std::unexpected("git_show error printing diff: " +
                (e ? std::string(e->message) : std::string("unknown error")));
        }

        return spill_long_output(std::move(result), tool_logs);
    };
    return t;
}
