#pragma once

#include "config.h"
#include "gui_app.h"
#include "agent.h"

#include <string>

struct ImFont;
extern ImFont* mono_font;

void render_config_tab(PrimaryAgent& tab, const Config& cfg, ImFont* mono_font);
void render_subagent_tab(SubAgent& tab, const Config& cfg, ImFont* mono_font);
void render_subagent_chat(SubAgent& tab, ImFont* mono_font);
void render_chat_ui(Agent& tab, bool& done);
void drain_pending(ChatUIState& ui, AsyncChatState& chat);
void render_content(const std::string& text, ImFont* mono_font = nullptr);
