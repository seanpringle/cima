#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <string>

extern std::atomic<bool> g_interrupted;

template <typename T> using Result = std::expected<T, std::string>;

struct Config {
    std::string api_base = "http://127.0.0.1:11000/v1";
    std::string api_key;
    std::string model = "deepseek-v4-flash";
    std::string system_prompt = "You are a helpful assistant.";
    std::string safe_dir;
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
    int max_tool_iterations = 100;
    int context_limit = 300000;       // model context window (tokens)
    int compact_threshold = 90;        // % that triggers compaction

    static Config from_env();
};
