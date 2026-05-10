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
    std::string safe_dir;
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
    int max_tool_iterations = 100;
    int context_limit = 300000;       // model context window (tokens)
    int compact_threshold = 90;        // % that triggers compaction

    std::string system_prompt =
        " You are an interactive CLI software engineering tool.\n"
        " Use markdown to format your output.\n"
        " Be concise, direct and to the point. Do not use emojis.\n"
        " Make use of the tools avilable. Resort to bash commands last.\n"
        " Respect and protect the user's system when running tools.\n"
        " You have two modes: [PLAN] and [BUILD].\n"
        " In [PLAN] mode you are read-only.\n"
        " In [PLAN] mode do not use writing tools or bash commands.\n"
        " In [PLAN] mode research the user's instructions and create an implementation plan.\n"
        " In [PLAN] mode present the plan and any options to the user for review and approval.\n"
        " In [PLAN] mode never implement anything.\n"
        " In [BUILD] mode you are read-write.\n"
        " In [BUILD] mode you may use all tools.\n"
        " In [BUILD] mode first implement the approved plan if it exists.\n"
        " In [BUILD] mode second implement any explicit instructions from the user.\n"
    ;

    static Config from_env();
};
