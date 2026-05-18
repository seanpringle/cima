#pragma once

#include "chat.h"
#include "config.h"
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
    int next_seq = 1;

    /// Create a DisplayEntry with the given type/text/streaming flag, assign
    /// next_seq, push to entries, and log non-streaming entries immediately.
    void push_entry(EntryType type, const std::string& text, bool streaming);

    /// Finalise the last streaming entry (mark non-streaming, log it).
    void finalize_streaming_entry();

    // Lifeguard shared_ptr + weak_ptr pair for async model-fetch threads.
    // The background thread captures a weak_ptr; if the tab is destroyed
    // the shared_ptr is reset and the weak_ptr expires, preventing UAF.
    std::shared_ptr<bool> tab_alive{std::make_shared<bool>(true)};

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

    // (workspace_path_buf removed — safe_dir locked to cwd)
};

int gui_main(const std::string& session_name);
