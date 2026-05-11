#include "tools.h"

#include <filesystem>
#include <git2.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// ===================================================================
// Safe recursive directory deletion (never follows symlinks)
// ===================================================================

void remove_all_safe(const std::filesystem::path& dir) {
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
// Sanitize a branch name for use as a filesystem directory component.
// ===================================================================

std::string sanitize_branch_name(const std::string& branch) {
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
// Check for uncommitted changes in the worktree
// ===================================================================

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
