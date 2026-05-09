#pragma once

#include "chat.h"
#include "config.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <future>
#include <mutex>
#include <string>
#include <vector>

enum class EntryType { UserText, Reasoning, Content, ToolCall };

struct DisplayEntry {
    EntryType type;
    std::string text;
    bool is_streaming = false;
    int seq = 0;
};

struct AsyncChatState {
    std::mutex mutex;
    std::vector<std::pair<std::string, OutputType>> pending;
    std::atomic<bool> running{false};
    std::future<Result<ChatResult>> future;
};

int gui_main(Config cfg);
