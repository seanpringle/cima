#pragma once

#include "gui_app.h"
#include "tools.h"

#include <set>
#include <string>
#include <vector>

struct ImFont;

struct ChatUIState {
    std::vector<DisplayEntry> entries;
    char input_buf[4096] = {};
    bool auto_scroll = true;
    bool request_cancel = false;
    char model_buf[256] = {};
    ImFont* mono_font = nullptr;
    Mode mode = Mode::Plan;
    int next_seq = 1;

    // Jobs
    std::set<std::string> open_job_windows;
};

void render_chat_ui(ChatUIState& ui, AsyncChatState& chat, ChatSession& session, bool& done);
