#pragma once

#include "config.h"

#include <cstdlib>
#include <chrono>
#include <curl/curl.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Path sandbox
// ---------------------------------------------------------------------------
Result<std::string> resolve_path(const std::string& raw_path,
    const std::string& safe_dir,
    const std::vector<std::string>& extra_allowed = {});

// ---------------------------------------------------------------------------
// ToolPermission — which permission category a tool belongs to
// ---------------------------------------------------------------------------
enum class ToolPermission { ReadOnly, Write, Internal };

// ---------------------------------------------------------------------------
// Tool
// ---------------------------------------------------------------------------
struct Tool {
    std::string name;
    std::string description;
    json parameters;
    ToolPermission permission = ToolPermission::Write;
    int timeout_sec = 0; // 0 = no timeout
    std::function<Result<std::string>(const json& args)> execute;
};

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------
class ToolRegistry {
  public:
    ToolRegistry() = default;

    void set_cancelled(CancellationToken t) { cancelled_ = std::move(t); }

    void add(Tool tool);
    void add_defaults(std::shared_ptr<std::string> safe_dir,
        const Config& config,
        bool include_write = true);

    // Convenience overload: accepts a plain string safe_dir (wraps in shared_ptr internally).
    void add_defaults(const std::string& safe_dir,
        const Config& config,
        bool include_write = true);

    json to_openai_tools() const;
    /// Return tools for OpenAI, filtered to only include tools whose names
    /// appear in \p only_these (if non-null).
    json to_openai_tools(const std::set<std::string>* only_these) const;
    Result<std::string> execute(const std::string& name, const std::string& args_json);

    const std::vector<Tool>& tools() const { return tools_; }

    /// Return the names of all registered tools with the given permission.
    std::set<std::string> tool_names_by_permission(ToolPermission perm) const;

  private:
    Tool* find(const std::string& name);
    CancellationToken cancelled_;
    std::vector<Tool> tools_;
};

// ---------------------------------------------------------------------------
// Git helpers
// ---------------------------------------------------------------------------

struct git_repository;

/// Open the git repository at or walking up from safe_dir.
/// Returns the repo handle or an error string.
/// Caller must git_repository_free() the handle on success.
Result<git_repository*> open_git_repo(const std::string& safe_dir);

/// Convert libgit2 status flags to porcelain v1 characters (index status).
char status_char_for_index(unsigned int flags);
/// Convert libgit2 status flags to porcelain v1 characters (workdir status).
char status_char_for_workdir(unsigned int flags);

/// Check whether a path within a git repository is ignored by .gitignore rules.
/// @param repo      An open git_repository*, or nullptr (returns false).
/// @param abs_path  Absolute filesystem path to check.
/// @param workdir   The repository worktree root (from git_repository_workdir()).
bool is_gitignored(git_repository* repo,
    const std::filesystem::path& abs_path,
    const std::filesystem::path& workdir);

/// Get the current git branch name at the given repository path.
/// Returns the branch name, or a description like "(detached HEAD at <hash>)"
/// if the repo is in detached HEAD state.
/// Returns an error if the path is not a git repository.
Result<std::string> get_current_git_branch(const std::string& repo_path);

// ---------------------------------------------------------------------------
// Web helper declarations (used by tool factories)
// ---------------------------------------------------------------------------

/// Callback for curl to write response body into a std::string.
size_t web_search_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
/// Progress callback for curl to support cancellation.
int web_search_progress_cb(
    void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

/// Shared HTTP GET helper.
Result<std::pair<std::string, long>> http_get(
    const std::string& url, int timeout_sec = 15, std::atomic<bool>* cancelled = nullptr);

/// Returns true if the scheme is http or https (case-insensitive).
bool is_valid_fetch_scheme(const std::string& url);

// ── DuckDuckGo rate limiter globals ──
extern std::chrono::steady_clock::time_point g_last_ddg_request;
extern std::mutex g_ddg_mutex;
inline constexpr std::chrono::milliseconds DDG_MIN_INTERVAL = std::chrono::milliseconds(1000);

// ── web_fetch cache globals ──
extern std::mutex g_fetch_cache_mutex;
extern std::unordered_map<std::string, std::string> g_fetch_cache;

// ---------------------------------------------------------------------------
// Tool factory declarations (used by ToolRegistry::add_defaults)
// ---------------------------------------------------------------------------
Tool make_list_files_tool(
    std::shared_ptr<std::string> safe_dir_ptr, const std::vector<std::string>& read_only_paths);
Tool make_read_file_lines_tool(
    std::shared_ptr<std::string> safe_dir_ptr, const std::vector<std::string>& read_only_paths);
Tool make_read_file_tool(
    std::shared_ptr<std::string> safe_dir_ptr, const std::vector<std::string>& read_only_paths);
Tool make_grep_files_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled = nullptr);
Tool make_write_file_tool(std::shared_ptr<std::string> safe_dir_ptr);
Tool make_edit_file_tool(std::shared_ptr<std::string> safe_dir_ptr);
Tool make_run_bash_tool(
    std::shared_ptr<std::string> safe_dir_ptr, int timeout, CancellationToken cancelled = nullptr);
Tool make_web_search_tool(const std::string& api_key,
    const std::string& engine_id,
    const std::string& endpoint_override,
    int timeout,
    CancellationToken cancelled = nullptr);
Tool make_web_fetch_tool(int timeout, CancellationToken cancelled = nullptr);
Tool make_git_status_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout);
Tool make_git_diff_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout);
Tool make_git_log_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout);
Tool make_git_add_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout);
Tool make_git_commit_tool(std::shared_ptr<std::string> safe_dir_ptr, int timeout);
Tool make_project_tree_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled = nullptr);
Tool make_delete_file_tool(std::shared_ptr<std::string> safe_dir_ptr);
Tool make_move_file_tool(std::shared_ptr<std::string> safe_dir_ptr);
Tool make_rename_file_tool(std::shared_ptr<std::string> safe_dir_ptr);

class SessionDB;
Tool make_query_session_tool(SessionDB& db);

class Wiki;

// ── Wiki tools ──
Tool make_list_wiki_pages_tool(Wiki& wiki);
Tool make_read_wiki_page_tool(Wiki& wiki);
Tool make_write_wiki_page_tool(Wiki& wiki);
Tool make_edit_wiki_page_tool(Wiki& wiki);
Tool make_delete_wiki_page_tool(Wiki& wiki);


