#pragma once

#include "config.h"
#include "gui_app.h"
#include "tools.h"
#include "types.h"

#include <string>
#include <vector>

struct ImFont;

void render_config_tab(TabInfo& tab, const Config& cfg, ImFont* mono_font);
void render_chat_ui(TabInfo& tab, bool& done);
void drain_pending(ChatUIState& ui, AsyncChatState& chat);
void render_content(const std::string& text, ImFont* mono_font = nullptr);
