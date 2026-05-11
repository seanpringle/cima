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
    TabType tab_type = TabType::Planner;
    int next_seq = 1;
    bool show_raw_popup = false;

};

struct TabInfo {
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<AsyncChatState> chat_state;
    ChatUIState ui_state;
    int id = 0;
    std::string title;
    TabType type;
};

int gui_main(Config cfg);
