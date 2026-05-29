#pragma once

struct PrimaryAgent;
struct SubAgent;

void render_history_tab(PrimaryAgent& tab);
void render_subagent_chat(SubAgent& tab);
void render_chat_ui(PrimaryAgent& tab, bool& done);
