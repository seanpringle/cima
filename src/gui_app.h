#pragma once

#include "chat.h"
#include "config.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <future>
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

    // Available models from the endpoint (populated by fetch_models)
    std::vector<std::string> available_models;
    bool models_loaded = false; // true once the initial fetch has been attempted
    std::unique_ptr<std::atomic<bool>> models_fetched{std::make_unique<std::atomic<bool>>(
        false)}; // publication flag (release/acquire synchronises the other fields)
    std::string models_error;

    // Tracks the async model-fetch so we can wait for completion before tab close
    std::future<void> models_future;

    std::deque<std::string> input_history;
    std::vector<char> input_buffer = {0};
};

struct TabInfo {
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<AsyncChatState> chat_state;
    ChatUIState ui_state;
    int id = 0;
    std::string title;
    std::string git_branch;
    std::string workspace_path;
};

int gui_main(Config cfg);
