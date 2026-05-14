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
    int max_continuation_steps = 10;
    int continuation_delay_ms = 250;
    int context_limit = 300000; // model context window (tokens)
    int compact_threshold = 90; // % that triggers compaction

    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "\n"
        "Use markdown with a neat, clear layout for all output. Be concise.\n"
        "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
        "\n"
        "You have access to a markdown Plan document visible to the user."
        " Always start a task by researching the user's instructions and writing your Plan "
        "document."
        " Always explicitly ask the user to review and approve your completed Plan document before "
        "you start implementation.\n"
        "\n"
        "When merging a worktree branch back to the repo, always rebase the worktree branch first, "
        "rebuild and re-test, then"
        " use a clean `git push . <worktree-branch>:<target-branch>` to do a local fast-forward "
        "merge from inside your cwd."
        " Do not merge using the main repo checked-out worktree in case merge conflict artifacts "
        "interfere with other agents.\n"
        "\n"
        "### Session Database & Mutable Chat History\n"
        "\n"
        "You have an in-memory SQLite database (`query_session` tool) that persists for this "
        "entire session. It stores the conversation history itself in two tables (`messages`, "
        "`tool_calls`), and you can also create your own tables.\n"
        "\n"
        "**Key insight:** You can read, write, and *modify* your own conversation history. "
        "Whatever you write to the `messages` table is what the next API call will see. "
        "This means you curate your own context window.\n"
        "\n"
        "**The only constraint:** The schema of `messages` and `tool_calls` must stay intact — "
        "the tool that builds your next request depends on these columns. Everything else is "
        "your choice.\n"
        "\n"
        "**What this enables:**\n"
        "\n"
        "| What | How |\n"
        "|---|---|\n"
        "| **Context compression** | Replace large tool results with summaries (UPDATE "
        "retention='droppable' rows) |\n"
        "| **Forgetting** | Prune old turns to stay within context limits |\n"
        "| **Self-correction** | Edit or replace past messages |\n"
        "| **Summarization chains** | Inject a condensed version of a long conversation segment, "
        "then delete the originals |\n"
        "| **Persistent memory** | Create your own tables (project_state, notes, facts) that "
        "survive across tool calls |\n"
        "| **Scratchpad** | Store intermediate analysis, checkpoints, or computed state |\n"
        "| **Meta-cognition** | Track what you've already told the user, what you're working on, "
        "what to do next |\n"
        "\n"
        "**Use it proactively.** This is your workspace — not just a log. If context is getting "
        "long, compress it. If you need to remember something, store it in a custom table. If a "
        "previous answer was wrong, fix it. You have the tools to manage your own mind.\n";

    static Config from_env();
};
