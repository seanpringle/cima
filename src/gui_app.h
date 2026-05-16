#pragma once

#include "chat.h"
#include "config.h"
#include "lsp/lsp_client.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct AsyncChatState {
    std::mutex mutex;
    std::vector<std::pair<std::string, OutputType>> pending;
    std::atomic<bool> running{false};
    std::future<Result<ChatResult>> future;
    CancellationToken cancelled = make_cancellation_token();
};

struct ImFont;

struct ChatUIState {
    std::vector<DisplayEntry> entries;
    bool auto_scroll = true;
    bool request_cancel = false;
    ImFont* mono_font = nullptr;
    int next_seq = 1;
    bool show_raw = false;
    /// Load entries from the JSON Lines log file into `entries`.
    /// Sets `next_seq` to max seq + 1.  Silently ignores corrupt lines.
    void load_chat_log(const std::string& path);

    /// Append a single entry to the JSON Lines log file.
    void append_chat_log_entry(const DisplayEntry& entry);

    // Available models from the endpoint (populated by fetch_models)
    std::vector<std::string> available_models;
    bool models_loaded = false; // true once the initial fetch has been attempted
    std::unique_ptr<std::atomic<bool>> models_fetched{std::make_unique<std::atomic<bool>>(
        false)}; // publication flag (release/acquire synchronises the other fields)
    std::string models_error;

    // Tracks the async model-fetch so we can wait for completion before tab close
    std::future<void> models_future;

    bool models_validated = false; // true once the render thread has applied auto-select

    // Compact button state
    bool compact_requested = false;
    bool compacting = false;
    std::future<Result<void>> compact_future;

    // Clear button state
    bool clear_requested = false;
    bool clearing = false;
    std::future<Result<void>> clear_future;

    std::deque<std::string> input_history;
    std::vector<char> input_buffer = {0};
    int cursor_pos = 0; // tracked by InputText callback for insert-at-cursor

    // Workspace path input state
    std::string workspace_path_buf; // local editor buffer, synced from session when inactive
};

struct TabInfo {
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<LspClient> lsp_client;
    std::unique_ptr<AsyncChatState> chat_state;
    ChatUIState ui_state;
    int id = 0;
    std::string title;          // Culture ship name (display label)
    std::string model_name;     // actual model name (shown in dropdown)
    std::string provider_name;  // which provider this tab belongs to
    std::string reasoning_effort; // per-tab reasoning effort override
    std::string git_branch;
    std::string workspace_path;

    /// Snippets from cima.json (shared across all tabs)
    const std::map<std::string, std::string>* snippets = nullptr;
};

int gui_main(Config cfg, const std::string& session_name, bool force = false);
