#pragma once

#include "config.h"
#include "gui_app.h"
#include "types.h"

#include <string>

// ── Mono font (set by gui_app.cpp during font loading) ──
struct ImFont;
extern ImFont* mono_font;

// ── Tool result rendering mode ──
enum class RenderToolResult {
    Plain,
    Diff,
};

/// Render markdown text via md4c into ImGui widgets.
void render_content(const std::string& text);

/// Render a single display entry (user text, reasoning, content, or tool call).
void render_display_entry(
    const ChatUIState& ui, const DisplayEntry& entry, size_t& i, const std::string& you);
