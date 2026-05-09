#pragma once

#include "gui_app.h"
#include "tools.h"

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
  Mode mode = Mode::Build;
};

void render_chat_ui(ChatUIState& ui, AsyncChatState& chat, ChatSession& session, bool& done);
