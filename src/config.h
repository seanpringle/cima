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

// ── Default knob values (can be overridden per-session via SessionData) ──
inline constexpr int kDefaultMaxToolIterations = 100;
inline constexpr int kDefaultSubagentTimeout  = 600;
inline constexpr int kDefaultBashTimeout      = 30;
inline constexpr int kDefaultGrepTimeout      = 10;
inline constexpr int kDefaultWebSearchTimeout = 15;
inline constexpr int kDefaultWebFetchTimeout  = 15;

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
    std::string description;                 // human-readable description (for prompt table)
    std::map<std::string, std::string> env; // extra env vars for stdio
    int timeout_sec = 60;
};

inline bool operator==(const McpEndpoint& a, const McpEndpoint& b) {
    return a.name == b.name && a.transport == b.transport && a.command == b.command &&
        a.args == b.args && a.cwd == b.cwd && a.url == b.url && a.api_key == b.api_key &&
        a.description == b.description && a.env == b.env && a.timeout_sec == b.timeout_sec;
}

/// A single subagent definition from cima.json.
struct SubagentConfig {
    std::string name;        // unique identifier, used as tab title
    std::string description; // shown in tool description
    bool read_only = false;  // if true, no write tools (file, git)
};

struct Provider {
    std::string name;     // unique identifier, e.g. "opencode.go"
    std::string api_base; // e.g. "https://api.opencode.go/v1"
    std::string api_key;
    std::string api_type = "openai"; // "openai" | "anthropic" (default for sessions)
    std::string model;            // default model for this provider
    std::string reasoning_effort; // reasoning effort (empty = not set / omit from API)
    std::vector<std::string> reasoning_efforts; // allowed values for the dropdown
    int context_limit = 300000;                 // model context window (tokens)
    int max_tokens = 0;                         // 0 = auto-derive from context_limit / 4
};

struct Config {
    std::vector<Provider> providers;
    std::vector<McpEndpoint> mcp_servers; // MCP endpoint definitions
    std::vector<std::string> read_only_paths;
    int context_limit = 300000;                  // model context window (tokens)
    std::map<std::string, std::string> snippets; // from cima.json
    std::vector<SubagentConfig> subagents;       // from cima.json

    // Font settings (empty paths = auto-detect via fontconfig)
    std::string font_sans; // path to sans-serif font file
    std::string font_mono; // path to monospace font file
    int font_size = 18;    // base font size in points (before display scaling)

    static std::string SYSTEM_PROMPT;
    static std::string SUBAGENT_SYSTEM_PROMPT;

    /// Load config from ~/.config/cima/cima.json, applying defaults for
    /// any missing fields.  Creates the file with defaults if it doesn't exist.
    static Config load();

    /// Path to the config file (~/.config/cima/cima.json).
    static std::filesystem::path config_file_path();

    /// Serialise the JSON-persisted fields (excludes system_prompt etc.).
    json to_json() const;
};

using ConfigPtr = std::shared_ptr<Config>;
