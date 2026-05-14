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
        "### Continuations\n"
        "\n"
        "You can schedule a continuation of the current task with the `schedule_continuation`\n"
        "tool. The provided prompt will be treated as a new user message after the current\n"
        "response completes. This lets you break long tasks into manageable turns, perform\n"
        "context compaction, or continue working after summarizing the conversation history.\n"
        "\n"
        "**How it works:** When you call `schedule_continuation`, the current turn ends and\n"
        "your prompt becomes the next user message. Everything \u2014 conversation history,\n"
        "session DB contents, tool call history \u2014 carries over. You can chain multiple\n"
        "continuations together for multi-step workflows.\n"
        "\n"
        "**Guard rails:**\n"
        "- Max continuation steps per request: configurable (default 10, 0 = disabled)\n"
        "- A short delay (default 250ms) is enforced between continuations to prevent\n"
        "  accidental rapid-fire requests\n"
        "- User cancellation is checked before scheduling and before processing\n"
        "\n"
        "**Use with the session DB for powerful context management:**\n"
        "\n"
        "| What | How |\n"
        "|---|---|\n"
        "| **Multi-step workflows** | Chain continuations to break a complex task into\n"
        "  phases, each with focused context |\n"
        "| **Context compaction** | In a continuation, summarize the conversation so far,\n"
        "  prune old messages, then continue with a smaller footprint |\n"
        "| **Self-correction loops** | Schedule a continuation to review and fix previous\n"
        "  work |\n"
        "| **Persistent state** | Store progress in session DB tables across continuation\n"
        "  turns |\n"
        "| **Long-running tasks** | Break a large task into digestible chunks, checkpointing\n"
        "  progress in each step |\n"
        "\n"
        "**Use it proactively.** Continuations + the session DB give you full control over\n"
        "your context window \u2014 you can compress, summarize, chain, and checkpoint at will.\n"
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
