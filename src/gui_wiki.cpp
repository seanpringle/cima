#include "gui_wiki.h"
#include "gui_chat.h"
#include "wiki.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

using namespace ImGui;

#include <functional>
#include <string>
#include <vector>

// ── Helpers shared by both sub-tabs ──

static void layout_left_right(Wiki& wiki, ImFont* mono_font,
    std::function<void()> render_left,
    std::function<void()> render_right) {
    auto padding = GetStyle().WindowPadding;
    auto space = GetContentRegionAvail();
    auto tl = GetCursorPos();
    float gap = GetStyle().ItemSpacing.x * 2.0f;
    float leftWidth = space.x * 0.4f - gap;
    auto leftPos = tl;
    auto leftSize = ImVec2(leftWidth, space.y);
    auto rightPos = ImVec2(tl.x + leftWidth + gap, tl.y);
    auto rightSize = ImVec2(-1, space.y);
    auto winPos = GetWindowPos();
    auto sepPosA = ImVec2(winPos.x + tl.x + leftWidth + (gap / 2), winPos.y + tl.y);
    auto sepPosB = ImVec2(sepPosA.x, winPos.y + tl.y + space.y);

    SetCursorPos(leftPos);
    BeginChild("##wiki-left", leftSize,
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    render_left();
    EndChild();

    GetWindowDrawList()->AddLine(sepPosA, sepPosB,
        GetColorU32(ImGuiCol_Separator), GetStyle().ChildBorderSize);

    SetCursorPos(rightPos);
    BeginChild("##wiki-right", rightSize,
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    render_right();
    EndChild();
}

void render_wiki_tab(Wiki& wiki, ImFont* mono_font) {
    // ── Fetch data ──
    static std::string selected_page;

    auto pages_result = wiki.list_pages();
    if (pages_result) {
        bool found = false;
        for (const auto& t : *pages_result) {
            if (t == selected_page) { found = true; break; }
        }
        if (!found) selected_page.clear();
    } else {
        selected_page.clear();
    }

    layout_left_right(wiki, mono_font,
        [&] {
            if (!pages_result) {
                TextDisabled("(wiki unavailable)");
            } else if (pages_result->empty()) {
                TextDisabled("(no pages)");
            } else {
                for (const auto& title : *pages_result) {
                    bool is_sel = (title == selected_page);
                    if (Selectable(title.c_str(), is_sel)) {
                        selected_page = title;
                    }
                    if (is_sel) SetItemDefaultFocus();
                }
            }
        },
        [&] {
            if (selected_page.empty()) {
                TextDisabled("Select a page from the left panel");
            } else {
                auto content_result = wiki.read_page(selected_page);
                if (!content_result) {
                    TextColored(ImColor(IM_COL32(255, 100, 100, 255)), "%s",
                        content_result.error().c_str());
                } else {
                    render_content(*content_result, mono_font);
                }
            }
        }
    );
}
