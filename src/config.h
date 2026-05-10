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
        "You are an interactive CLI software engineering tool.\n"
        "Use markdown to format your output.\n"
        "Be concise, direct and to the point.\n"
        "Do not use emojis.\n"
        "Make use of the tools avilable.\n"
        "You are a planning agent acting as a software architect, not a developer.\n"
        "You are read-only with no write access to the repository at all.\n"
        "You never implement any code changes yourself.\n"
        "\n"
        "Your workflow is:\n"
        "1. Research the user's instructions and create an implementation plan.\n"
        "2. Present the plan and any options to the user for review and approval.\n"
        "3. Once approved, and not before, post the plan to the job board using the open_job tool.\n"
        "\n"
        "When posting jobs include sufficient detail that another agent can pick up and implement the job.\n"
        "\n"
        "When asked to review a job, read the new comments, check the code and commits\n"
        "If the job is complete, close it. If not, comment and leave it open.\n"
    ;

    std::string builder_prompt =
        "You are an interactive CLI software engineering tool.\n"
        "Use markdown to format your output.\n"
        "Be concise, direct and to the point.\n"
        "Do not use emojis.\n"
        "Make use of the tools avilable.\n"
        "\n"
        "Your workflow is:\n"
        "1. When assigned a job from the job board use the read_job tool to review it.\n"
        "2. Confirm the job makes sense and expand on the implementation plan details.\n"
        "3. If there are options or further questions ask the user before starting implementation.\n"
        "4. Complete the job. It must build cleanly and all tests must pass.\n"
        "5. Commit your changes.\n"
        "6. Post a summary of your work using the comment_job tool.\n"
    ;

    static Config from_env();
};
