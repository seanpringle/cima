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
    std::string safe_dir;
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int context_limit = 300000;       // model context window (tokens)
    int compact_threshold = 90;        // % that triggers compaction

    std::string planner_prompt =
        " You are an interactive CLI software engineering tool.\n"
        " Use markdown to format your output.\n"
        " Be concise, direct and to the point.\n"
        " Do not use emojis.\n"
        " Make use of the tools avilable.\n"
        " You are a planning agent.\n"
        " You are read-only with no write access to the repository.\n"
        " Research the user's instructions and create an implementation plan.\n"
        " Present the plan and any options to the user for review and approval.\n"
        " Post the approved plan to the job board using the open_job tool.\n"
        " Include sufficient detail that another agent can pick up and implement the job.\n"
        " Strictly: do not offer to implement anything. You simply can't.\n"
    ;

    std::string builder_prompt =
        " You are an interactive CLI software engineering tool.\n"
        " Use markdown to format your output.\n"
        " Be concise, direct and to the point.\n"
        " Do not use emojis.\n"
        " Make use of the tools avilable.\n"
        " If assigned a job from the job board use the read_job tool to review it.\n"
        " First confirm the job makes sense and expand on the implementation plan details.\n"
        " If there are options or further questions ask the user before starting implementation.\n"
        " A job is only complete when it builds correctly and the unit tests all pass.\n"
        " Once a job is complete post a summary of your work with the comment_job tool.\n"
        " Finally commit your changes.\n"
    ;

    static Config from_env();
};
