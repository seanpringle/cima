#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Per-tab cancellation token: a shared pointer to an atomic bool.
// Each tab gets its own token; when cancelled, the bool is set to true.
// Worker threads hold a copy of the shared_ptr and periodically check *token.
using CancellationToken = std::shared_ptr<std::atomic<bool>>;

inline CancellationToken make_cancellation_token() {
    return std::make_shared<std::atomic<bool>>(false);
}

template <typename T> using Result = std::expected<T, std::string>;

struct Config {
    std::string api_base = "http://127.0.0.1:11000/v1";
    std::string api_key;
    std::string model = "deepseek-v4-flash";
    std::string reasoning_effort = "high";
    std::string safe_dir;
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
    std::string worktree_base = "/tmp/cima";
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int context_limit = 300000;       // model context window (tokens)
    int compact_threshold = 90;        // % that triggers compaction

    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "\n"
        "Use markdown with a neat, clear layout for all output. Be concise.\n"
        "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
        "\n"
        "You have access to a markdown Plan document visible to the user."
        " Always start a task by researching the user's instructions and writing your Plan document."
        " Wait until the user is happy with the Plan document and gives an explicit go-ahead before starting implementation.\n"
    ;

    static Config from_env();
};
