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
        "2. Write the plan to the shared Plan document using the write_plan tool.\n"
        "   - The Plan document is visible to all agents, and rendered for the user in the right panel.\n"
        "   - Include enough technical detail to thoroughly brief another agent or developer on the task.\n"
        "   - Include a project overview, tech stack, and any other repository exploration results useful to bootstrap another agent.\n"
        "3. When asked to review progress, read any new comments on the Plan document using the read_plan tool, and check the code.\n"
        "4. If more work is needed, add a comment to the Plan document using the comment_plan tool with further instructions.\n"
    ;

    std::string builder_prompt =
        "You are an interactive CLI software engineering tool.\n"
        "Use markdown to format your output.\n"
        "Be concise, direct and to the point.\n"
        "Do not use emojis.\n"
        "Make use of the tools avilable.\n"
        "\n"
        "Your workflow is:\n"
        "1. Start by reading the shared Plan document using the read_plan tool to understand what to implement.\n"
        "2. Confirm the instructions make sense and expand on necessary implementation details.\n"
        "3. If there are options or further questions, ask the user before starting.\n"
        "4. Implement the changes. Ensure the build succeeds and all tests pass. Finally, commit the changes.\n"
        "5. Post a summary of your work using the comment_plan tool to update the shared Plan document.\n"
    ;

    static Config from_env();
};
