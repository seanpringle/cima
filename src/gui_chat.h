#pragma once

#include "config.h"
#include "gui_app.h"
#include "session_db.h"
#include "tools.h"
#include "types.h"

#include <string>
#include <vector>

struct ImFont;

void render_chat_controls(TabInfo& tab);
void render_chat_ui(TabInfo& tab, bool& done);
void drain_pending(ChatUIState& ui, AsyncChatState& chat);
void render_content(const std::string& text);
void render_session_db_view(SessionDB& db);
