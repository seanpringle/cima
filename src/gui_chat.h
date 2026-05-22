#pragma once

#include "config.h"
#include "gui_app.h"
#include "agent.h"

#include <string>

struct ImFont;
extern ImFont* mono_font;

void render_config_tab(PrimaryAgent& tab);
void render_history_tab(PrimaryAgent& tab);
void render_subagent_tab(SubAgent& tab);
void render_subagent_chat(SubAgent& tab);
void render_chat_ui(PrimaryAgent& tab, bool& done);
void render_content(const std::string& text);
