#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

extern std::atomic<bool> g_interrupted;

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
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int context_limit = 300000;       // model context window (tokens)
    int compact_threshold = 90;        // % that triggers compaction

    std::string system_prompt =
        "You are an AI coding assistant with access to tools for file editing, git operations,\n"
        "web search, and a shared Plan document. Use markdown for your output. Be concise.\n"
    ;

    static Config from_env();
};
