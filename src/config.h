#pragma once

#include <atomic>
#include <expected>
#include <filesystem>
#include <map>
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
    int context_limit = 300000; // model context window (tokens)
    std::map<std::string, std::string> snippets; // from cima.json

    // Tool timeouts (seconds, 0 = no timeout)
    int bash_timeout = 30;
    int project_tree_timeout = 5;
    int git_status_timeout = 10;
    int git_diff_timeout = 10;
    int git_log_timeout = 10;
    int git_add_timeout = 10;
    int git_commit_timeout = 10;
    int grep_timeout = 10;
    int web_search_timeout = 15;
    int web_fetch_timeout = 15;

    std::string system_prompt =
        "You are an AI coding assistant.\n"
        "\n"
        "### Session Database (Scratch Space)\n"
        "\n"
        "You have an in-memory SQLite database (`query_session` tool) that you can use\n"
        "to store structured data across tool calls. Create tables, insert data, and\n"
        "query results freely — it's scratch space.\n"
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
