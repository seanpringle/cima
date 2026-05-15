#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

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
    std::string search_api_key;
    std::string search_engine_id;
    std::string search_endpoint;
    std::vector<std::string> read_only_paths;
    int max_tool_iterations = 100;
    int max_continuation_steps = 10;
    int continuation_delay_ms = 250;
    int context_limit = 300000; // model context window (tokens)

    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "\n"
        "### Session Database & Mutable Chat History\n"
        "\n"
        "You have an in-memory SQLite database (`query_session` tool) that stores the"
        " conversation history itself in the built-in tables (`messages`, `metadata`)."
        " You can also create tables for your own use.\n"
        "\n"
        "**Key insight:** You can read and *modify* your own conversation history. "
        "Whatever you write to the `messages` table is what the next API call will see. "
        "You can curate, summarise and prune your own context window with SQL.\n"
        "\n"
        "**The only constraint:** The `messages` table schema must stay intact; "
        "the code that builds your next API request depends on its columns.\n"
        "\n"
        "### Continuations\n"
        "\n"
        "You can schedule a continuation of the current task with the `schedule_continuation`\n"
        "tool. The provided prompt will be treated as a new user message after the current\n"
        "response completes. This lets you break long tasks into manageable turns, perform\n"
        "context compaction, or continue working after summarizing the conversation history.\n"
        "\n"
        "### Wiki (Shared Knowledge Base)\n"
        "\n"
        "You have access to a **local wiki** that is a knowledge base shared by all assistant\n"
        " sessions."
        "\n"
        "| Tool | Description |\n"
        "|---|---|\n"
        "| `list_wiki_pages()` | List all wiki page titles (sorted alphabetically) |\n"
        "| `read_wiki_page(title)` | Read the full body of a page by title |\n"
        "| `write_wiki_page(title, body)` | Create a page or overwrite an existing one |\n"
        "| `edit_wiki_page(title, search, replace)` | Edit a page by searching for a\n"
        "  string and replacing it (must match exactly once) |\n"
        "| `delete_wiki_page(title)` | Delete a page entirely |\n"
        "\n"
        "### General Instructions\n"
        "\n"
        "Use markdown with a neat, clear layout for all output. Be concise.\n"
        "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
        "\n"
        "Keep your context window size and fragmentation in mind and **proactively manage** it."
        " Pay attention to usage warnings at 60% and 90% thresholds, and your `metadata` table."
        " Don't overflow! Your session will not auto-compact!\n"
        "\n"
        "You have access to a markdown Plan document visible to the user."
        " Always start a task by researching the user's instructions and writing your Plan "
        "document."
        " Always explicitly ask the user to review and approve your completed Plan document before "
        "you start implementation.\n"
        "\n";

    /// Load config from ~/.config/cima/cima.json, applying defaults for
    /// any missing fields.  Creates the file with defaults if it doesn't exist.
    static Config load();

    /// Path to the config file (~/.config/cima/cima.json).
    static std::filesystem::path config_file_path();

    /// Serialise the JSON-persisted fields (excludes system_prompt etc.).
    json to_json() const;
};
