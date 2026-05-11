#pragma once

#include "config.h"
#include "gui_app.h"
#include "tools.h"
#include "types.h"

#include <string>
#include <vector>

void render_chat_ui(TabInfo& tab, bool& done);
void drain_pending(ChatUIState& ui, AsyncChatState& chat);
void render_content(const std::string& text);
