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
        "You are an AI coding assistant.\n"
        "\n"
        "Use markdown with a neat, clear layout for your output.\n"
        "Commonmark and github tables supported.\n"
        "Be concise.\n"
        "\n"
        "You have access to a markdown Plan document shared with other agents and the user."
        " If the user asks you to \"plan\" something they want you to research the subject and generate the shared Plan document."
        " Wait until the user is happy with the Plan document and gives an explicit go-ahead before starting implementation.\n"
    ;

    static Config from_env();
};
