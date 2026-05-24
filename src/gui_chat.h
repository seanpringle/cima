#pragma once

#include "config.h"
#include "gui_app.h"
#include "agent.h"
#include "gui_markdown.h"

#include <string>

void render_history_tab(PrimaryAgent& tab);
void render_subagent_tab(SubAgent& tab);
void render_subagent_chat(SubAgent& tab);
void render_chat_ui(PrimaryAgent& tab, bool& done);

/// Check for pending ask_user requests and render a centered modal popup.
/// Returns true while the modal is open (caller should suppress input).
bool render_ask_user_modal(AsyncChatState& chat_state, ChatUIState& ui_state);
