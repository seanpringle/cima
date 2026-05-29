#include "tools.h"

#include <git2.h>

// Initialize libgit2 at module load time.
// git_libgit2_init() is ref-counted and safe to call multiple times.
static const bool g_git_init = (git_libgit2_init(), true);

// ===================================================================
// Git helpers
// ===================================================================

// Open the git repository at or walking up from safe_dir.
// Returns the repo handle or an error string.
// Caller must git_repository_free() the handle on success.
Result<git_repository*> open_git_repo(const std::string& safe_dir) {
    git_repository* repo = nullptr;
    int err = git_repository_open_ext(&repo, safe_dir.c_str(), GIT_REPOSITORY_OPEN_CROSS_FS, nullptr);
    if (err != 0) {
        const git_error* e = git_error_last();
        return std::unexpected("not a git repository: " + (e ? std::string(e->message) : std::string("unknown error")));
    }
    return repo;
}

// Convert libgit2 status flags to porcelain v1 characters.
// Returns the character for the index (staging area) status.
// Porcelain v1: ' ' = clean, 'M' = modified, 'A' = added, 'D' = deleted,
// 'R' = renamed, 'T' = type change.
// Special case: if the file is untracked (GIT_STATUS_WT_NEW alone),
// both index and worktree show '?'.
char status_char_for_index(unsigned int flags) {
    if (flags & GIT_STATUS_INDEX_NEW)
        return 'A';
    if (flags & GIT_STATUS_INDEX_MODIFIED)
        return 'M';
    if (flags & GIT_STATUS_INDEX_DELETED)
        return 'D';
    if (flags & GIT_STATUS_INDEX_RENAMED)
        return 'R';
    if (flags & GIT_STATUS_INDEX_TYPECHANGE)
        return 'T';
    // If the only change is untracked in the worktree, show '?' in index too
    if (flags & GIT_STATUS_WT_NEW &&
        !(flags &
            (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE))) {
        return '?';
    }
    return ' ';
}

// Returns the character for the working tree status.
// Porcelain v1: ' ' = clean, 'M' = modified, 'D' = deleted, '?' = untracked,
// '!' = ignored, 'U' = conflicted.
char status_char_for_workdir(unsigned int flags) {
    if (flags & GIT_STATUS_WT_NEW)
        return '?';
    if (flags & GIT_STATUS_WT_MODIFIED)
        return 'M';
    if (flags & GIT_STATUS_WT_DELETED)
        return 'D';
    if (flags & GIT_STATUS_WT_RENAMED)
        return 'R';
    if (flags & GIT_STATUS_WT_TYPECHANGE)
        return 'T';
    if (flags & GIT_STATUS_IGNORED)
        return '!';
    if (flags & GIT_STATUS_CONFLICTED)
        return 'U';
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
    auto cleanup = std::unique_ptr<git_repository, decltype(&git_repository_free)>(repo, git_repository_free);

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
        return std::unexpected("failed to get HEAD: " + (e ? std::string(e->message) : std::string("unknown error")));
    }
    auto head_cleanup = std::unique_ptr<git_reference, decltype(&git_reference_free)>(head, git_reference_free);

    // Check if HEAD is a branch or detached
    if (git_reference_is_branch(head)) {
        const char* name = nullptr;
        err = git_branch_name(&name, head);
        if (err != 0 || !name) {
            const git_error* e = git_error_last();
            return std::unexpected("failed to get branch name: " + (e ? std::string(e->message) : std::string("unknown error")));
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
// Gitignore helpers
// ===================================================================

/// Check whether a path within a git repository is ignored by .gitignore rules.
/// @param repo      An open git_repository*, or nullptr (returns false).
/// @param abs_path  Absolute filesystem path to check.
/// @param workdir   The repository worktree root (from git_repository_workdir()).
bool is_gitignored(git_repository* repo, const std::filesystem::path& abs_path, const std::filesystem::path& workdir) {
    if (!repo)
        return false;

    std::error_code ec;
    auto rel = std::filesystem::relative(abs_path, workdir, ec);
    if (ec)
        return false;

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
