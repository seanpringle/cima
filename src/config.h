#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Per-tab cancellation token: a shared pointer to an atomic bool.
// Each tab gets its own token; when cancelled, the bool is set to true.
// Worker threads hold a copy of the shared_ptr and periodically check *token.
using CancellationToken = std::shared_ptr<std::atomic<bool>>;

inline CancellationToken make_cancellation_token() {
    return std::make_shared<std::atomic<bool>>(false);
}

template <typename T> using Result = std::expected<T, std::string>;

/// A single MCP endpoint definition from cima.json.
struct McpEndpoint {
    std::string name;                // unique id, e.g. "my-filesystem-server"
    std::string transport = "stdio"; // "stdio" or "streamable-http"
    // For stdio:
    std::string command;           // e.g. "npx", "uvx", "/path/to/server"
    std::vector<std::string> args; // e.g. ["-y", "@modelcontextprotocol/server-filesystem"]
    std::string cwd;               // working directory (empty = inherit)
    // For HTTP:
    std::string url;     // e.g. "http://localhost:3100/mcp"
    std::string api_key; // Bearer token for Authorization header
    // Common:
    std::map<std::string, std::string> env; // extra env vars for stdio
    int timeout_sec = 60;
};

// ── exec_ro / exec_rw whitelists ──
// Read-only commands — no filesystem modification
inline const std::set<std::string> exec_ro_allowed_commands = {
    "ls", "cat", "head", "tail", "grep",
    "cut", "tr", "wc", "pwd", "stat",
    "file", "find", "diff", "strings",
    "which", "date", "nl",
    "sort", "uniq", "comm",
    "basename", "dirname", "readlink", "realpath",
    "printf", "fold", "expand", "unexpand"
};

// Read-write commands — can modify the filesystem.
// Note: awk and sed are intentionally excluded because they have
// built-in system() calls (e.g. awk's system(), sed's 'e' flag)
// that allow arbitrary shell execution from within the script,
// bypassing the path-argument sandbox.
inline const std::set<std::string> exec_rw_allowed_commands = {
    "mkdir", "rmdir", "rm", "mv", "cp",
    "patch", "touch", "ln"
};

inline bool operator==(const McpEndpoint& a, const McpEndpoint& b) {
    return a.name == b.name && a.transport == b.transport && a.command == b.command &&
        a.args == b.args && a.cwd == b.cwd && a.url == b.url && a.api_key == b.api_key &&
        a.env == b.env && a.timeout_sec == b.timeout_sec;
}

/// A single subagent definition from cima.json.
struct SubagentConfig {
    std::string name;        // unique identifier, used as tab title
    std::string description; // shown in tool description
    bool read_only = false;  // if true, no write tools (file, git)
};

/// A single custom command tool definition from cima.json.
struct CmdToolConfig {
    std::string name;        // used as "cmd_<name>" tool name
    std::string description; // shown in tool description
    std::string command;     // predefined bash command to execute
};

struct Provider {
    std::string name;     // unique identifier, e.g. "opencode.go"
    std::string api_base; // e.g. "https://api.opencode.go/v1"
    std::string api_key;
    std::string model;            // default model for this provider
    std::string reasoning_effort; // reasoning effort (empty = not set / omit from API)
    std::vector<std::string> reasoning_efforts; // allowed values for the dropdown
    int context_limit = 300000;                 // model context window (tokens)
};

struct Config {
    std::vector<Provider> providers;
    std::vector<McpEndpoint> mcp_servers; // MCP endpoint definitions
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int context_limit = 300000;                  // model context window (tokens)
    std::map<std::string, std::string> snippets; // from cima.json
    std::vector<SubagentConfig> subagents;       // from cima.json
    std::vector<CmdToolConfig> cmd_tools;        // from cima.json

    // Tool timeouts (seconds, 0 = no timeout)
    int subagent_timeout = 600;
    int bash_timeout = 30;
    int project_tree_timeout = 5;
    int git_status_timeout = 10;
    int git_diff_timeout = 10;
    int git_log_timeout = 10;
    int git_add_timeout = 10;
    int git_commit_timeout = 10;
    int grep_timeout = 10;
    int web_search_timeout = 15;
    int web_fetch_timeout = 15;


    // ── exec_ro / exec_rw tools ──
    int exec_ro_timeout = 30;
    int exec_rw_timeout = 30;

    // ── CMake tools ──
    bool cmake_enabled = false; // user-facing toggle (like bash_enabled)
    int cmake_configure_timeout = 120; // cmake configure can be slow
    int cmake_build_timeout = 300;     // builds can take minutes
    int cmake_ctest_timeout = 300;     // test suites can be long

    // Font settings (empty paths = auto-detect via fontconfig)
    std::string font_sans; // path to sans-serif font file
    std::string font_mono; // path to monospace font file
    int font_size = 18;    // base font size in points (before display scaling)

    static std::string SYSTEM_PROMPT;
    static std::string SUBAGENT_SYSTEM_PROMPT;
    static std::string CMAKE_PROMPT_SNIPPET;

    /// Load config from ~/.config/cima/cima.json, applying defaults for
    /// any missing fields.  Creates the file with defaults if it doesn't exist.
    static Config load();

    /// Path to the config file (~/.config/cima/cima.json).
    static std::filesystem::path config_file_path();

    /// Serialise the JSON-persisted fields (excludes system_prompt etc.).
    json to_json() const;
};

extern Config cfg;
