#pragma once

#include "chat.h"
#include "config.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct AsyncChatState {
    std::mutex mutex;
    std::vector<std::pair<std::string, OutputType>> pending;
    std::atomic<bool> running{false};
    std::future<Result<ChatResult>> future;
};

struct ImFont;

struct ChatUIState {
    std::vector<DisplayEntry> entries;
    char input_buf[4096] = {};
    bool auto_scroll = true;
    bool request_cancel = false;
    char model_buf[256] = {};
    char title_buf[256] = {};
    ImFont* mono_font = nullptr;
    int next_seq = 1;
    bool show_raw = false;

    // Available models from the endpoint (populated by fetch_models)
    std::vector<std::string> available_models;
    bool models_loaded = false;      // true once the initial fetch has been attempted
    bool models_fetched = false;     // true once the fetch actually completed
    std::string models_error;

};

struct TabInfo {
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<AsyncChatState> chat_state;
    ChatUIState ui_state;
    int id = 0;
    std::string title;
};

int gui_main(Config cfg);
