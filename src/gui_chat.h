#pragma once

#include "config.h"
#include "gui_app.h"
#include "tools.h"
#include "types.h"

#include <string>
#include <vector>

struct ImFont;
class Notes;

void render_config_tab(TabInfo& tab, ImFont* mono_font);
void render_chat_ui(TabInfo& tab, bool& done);
void drain_pending(ChatUIState& ui, AsyncChatState& chat);
void render_content(const std::string& text);
void render_notes_tab(Notes& notes, ImFont* mono_font);
