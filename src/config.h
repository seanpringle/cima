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
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
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

    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "Use markdown with a neat, clear layout for all output. Be concise.\n"
        "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
        "\n";

    /// Load config from ~/.config/cima/cima.json, applying defaults for
    /// any missing fields.  Creates the file with defaults if it doesn't exist.
    static Config load();

    /// Path to the config file (~/.config/cima/cima.json).
    static std::filesystem::path config_file_path();

    /// Serialise the JSON-persisted fields (excludes system_prompt etc.).
    json to_json() const;
};
