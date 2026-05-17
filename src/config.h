#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
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
    std::string name;                           // unique id, e.g. "my-filesystem-server"
    std::string transport = "stdio";            // "stdio" or "streamable-http"
    // For stdio:
    std::string command;                        // e.g. "npx", "uvx", "/path/to/server"
    std::vector<std::string> args;              // e.g. ["-y", "@modelcontextprotocol/server-filesystem"]
    std::string cwd;                            // working directory (empty = inherit)
    // For HTTP:
    std::string url;                            // e.g. "http://localhost:3100/mcp"
    std::string api_key;                        // Bearer token for Authorization header
    // Common:
    std::map<std::string, std::string> env;     // extra env vars for stdio
    int timeout_sec = 60;
};

inline bool operator==(const McpEndpoint& a, const McpEndpoint& b) {
    return a.name == b.name
        && a.transport == b.transport
        && a.command == b.command
        && a.args == b.args
        && a.cwd == b.cwd
        && a.url == b.url
        && a.api_key == b.api_key
        && a.env == b.env
        && a.timeout_sec == b.timeout_sec;
}

/// A single provider definition from cima.json.
struct Provider {
    std::string name;               // unique identifier, e.g. "opencode.go"
    std::string api_base;           // e.g. "https://api.opencode.go/v1"
    std::string api_key;
    std::string model;              // default model for this provider
    std::string reasoning_effort = "high";
    int context_limit = 300000;     // model context window (tokens)
};

struct Config {
    std::vector<Provider> providers;
    std::vector<McpEndpoint> mcp_servers;   // MCP endpoint definitions
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int context_limit = 300000; // model context window (tokens)
    std::map<std::string, std::string> snippets; // from cima.json

    // Tool timeouts (seconds, 0 = no timeout)
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

    // ── CMake tool timeouts ──
    int cmake_configure_timeout = 120;  // cmake configure can be slow
    int cmake_build_timeout = 300;      // builds can take minutes
    int cmake_ctest_timeout = 300;      // test suites can be long

    // Font settings (empty paths = auto-detect via fontconfig)
    std::string font_sans;                   // path to sans-serif font file
    std::string font_mono;                   // path to monospace font file
    int font_size = 18;                      // base font size in points (before display scaling)

    /// CMake prompt snippet — appended to the system prompt only when
    /// CMakeLists.txt exists in the workspace.
    static constexpr const char* CMAKE_PROMPT_SNIPPET = R"(
## CMake tools

`cmake_configure(head=H, tail=T)` configures the project (generates compile_commands.json).
`cmake_build(head=H, tail=T)` builds the project.
`cmake_ctest(head=H, tail=T)` runs the test suite.
All return raw output with optional head/tail trimming.
)";
    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "Use markdown with a neat, clear and concise layout for all output.\n"
        "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
        "\n"
        "## Plan tools\n"
        "\n"
        "You have a **Plan document** shared with the user. When given a task, research"
        " it thoroughly and write your Plan with `write_plan()`."
        " Ask the user to review and approve your Plan before implementation.\n"
        "Go back and check your Plan at any time with `read_plan()`.\n"
        "\n"
        "## Wiki tools\n"
        "\n"
        "The wiki is a list of markdown documents shared with other coding asistants and the"
        " user. Access with `list_wiki_pages()`, `read_wiki_page()`, `write_wiki_page()`, and"
        " `edit_wiki_page()` tools. Note: the wiki is not file-based; only these tools can"
        " access it.\n"
        "\n";

    /// Load config from ~/.config/cima/cima.json, applying defaults for
    /// any missing fields.  Creates the file with defaults if it doesn't exist.
    static Config load();

    /// Path to the config file (~/.config/cima/cima.json).
    static std::filesystem::path config_file_path();

    /// Serialise the JSON-persisted fields (excludes system_prompt etc.).
    json to_json() const;
};
