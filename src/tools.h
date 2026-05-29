#pragma once

#include "config.h"

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
Result<std::string> resolve_path(const std::string& raw_path, const std::string& safe_dir, const std::vector<std::string>& extra_allowed = {});

// ---------------------------------------------------------------------------
// ToolPermission — which permission category a tool belongs to
// ---------------------------------------------------------------------------
enum class ToolPermission { ReadOnly, Write, Internal };

// Callback invoked after a write_file or edit_file successfully modifies
// a file on disk.  The argument is the resolved absolute path of the file.
using FileModifiedCallback = std::function<void(const std::string& path)>;

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
    void add_defaults(
        std::shared_ptr<std::string> safe_dir, const Config& config, bool include_write = true, FileModifiedCallback on_file_modified = nullptr);

    // Convenience overload: accepts a plain string safe_dir (wraps in shared_ptr internally).
    void add_defaults(const std::string& safe_dir, const Config& config, bool include_write = true, FileModifiedCallback on_file_modified = nullptr);

    json to_openai_tools() const;
    /// Return tools for OpenAI, filtered to only include tools whose names
    /// appear in \p only_these (if non-null).
    json to_openai_tools(const std::set<std::string>* only_these) const;
    /// Return tools for Anthropic Messages API (input_schema instead of parameters).
    json to_anthropic_tools() const;
    json to_anthropic_tools(const std::set<std::string>* only_these) const;
    Result<std::string> execute(const std::string& name, const std::string& args_json);

    const std::vector<Tool>& tools() const { return tools_; }

    /// Return the names of all registered tools with the given permission.
    std::set<std::string> tool_names_by_permission(ToolPermission perm) const;

    /// Return true if a tool with the given name is registered.
    bool has(const std::string& name) const;

    /// Return the names of all registered tools.
    std::vector<std::string> tool_names() const;

    /// Remove a tool by name.  Returns true if the tool was found and removed.
    bool remove(const std::string& name);

  private:
    Tool* find(const std::string& name);
    CancellationToken cancelled_;
    mutable std::mutex mutex_;
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
bool is_gitignored(git_repository* repo, const std::filesystem::path& abs_path, const std::filesystem::path& workdir);

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
int web_search_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);

/// Shared HTTP GET helper.
Result<std::pair<std::string, long>> http_get(const std::string& url, int timeout_sec = 15, std::atomic<bool>* cancelled = nullptr);

/// Returns true if the scheme is http or https (case-insensitive).
bool is_valid_fetch_scheme(const std::string& url);

// ── DuckDuckGo rate serialization and rate-limiting ──
extern std::mutex g_ddg_mutex;

// ── web_fetch cache globals ──
extern std::mutex g_fetch_cache_mutex;
extern std::unordered_map<std::string, std::string> g_fetch_cache;

// ── DDG HTML search interface ──

/// HTTP POST with URL-encoded form data (used for DDG HTML search).
Result<std::pair<std::string, long>> http_post_form(
    const std::string& url, const std::string& form_data, int timeout_sec, std::atomic<bool>* cancelled);

/// Parse DuckDuckGo HTML search results and return a formatted numbered list
/// of titles, snippets, and URLs.
Result<std::string> ddg_html_parse(const std::string& html);

/// Extract the real destination URL from a DDG redirect URL
/// (//duckduckgo.com/l/?uddg=<url-encoded>&rut=...).
/// Returns the decoded URL, or empty if parsing fails.
std::string extract_uddg_url(const std::string& ddg_url);

// ---------------------------------------------------------------------------
// Tool factory declarations (used by ToolRegistry::add_defaults)
// ---------------------------------------------------------------------------
Tool make_read_file_tool(std::shared_ptr<std::string> safe_dir_ptr, const std::vector<std::string>& read_only_paths);
Tool make_grep_files_tool(const Config& config,
    std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled = nullptr);
Tool make_find_files_tool(const Config& config,
    std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    int timeout,
    CancellationToken cancelled = nullptr);
Tool make_write_file_tool(std::shared_ptr<std::string> safe_dir_ptr, FileModifiedCallback on_file_modified = nullptr);
Tool make_edit_file_tool(std::shared_ptr<std::string> safe_dir_ptr, FileModifiedCallback on_file_modified = nullptr);
Tool make_run_bwrap_tool(const Config& config,
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout,
    CancellationToken cancelled = nullptr,
    bool read_only = false,
    bool allow_network = false);
Tool make_web_search_tool(const Config& config, int timeout, CancellationToken cancelled = nullptr);
Tool make_web_fetch_tool(const Config& config, int timeout, CancellationToken cancelled = nullptr);

class ChatSession;   // forward decl for SubagentLookup
class SkillRegistry; // forward decl for skill tool

struct PrimaryAgent;

// ── Subagent tool ──
Tool make_call_subagent_tool(PrimaryAgent&, const std::vector<SubagentConfig>& subagent_configs = {}, int timeout_sec = 600);

// ── Skill tool ──
Tool make_load_skill_tool(SkillRegistry& registry, ChatSession& session);
