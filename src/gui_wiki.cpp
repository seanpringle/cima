#include "gui_wiki.h"
#include "gui_chat.h"
#include "wiki.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

using namespace ImGui;

#include <string>
#include <vector>

void render_wiki_tab(Wiki& wiki, ImFont* mono_font) {
    auto padding = GetStyle().WindowPadding;
    auto space = GetContentRegionAvail();

    // ── Fetch page list ──
    // We fetch every frame so that tool-created pages appear immediately.
    // This is fine because list_pages() is cheap (SELECT on an SQLite table).
    static std::string selected_page;
    auto pages_result = wiki.list_pages();

    // Validate selected_page still exists
    if (pages_result) {
        bool found = false;
        for (const auto& t : *pages_result) {
            if (t == selected_page) {
                found = true;
                break;
            }
        }
        if (!found) {
            selected_page.clear();
        }
    } else {
        selected_page.clear();
    }

    // ── Layout: 40% left, 60% right ──
    {
        auto canvas = GetContentRegionAvail();
        auto tl = GetCursorPos();
        float gap = GetStyle().ItemSpacing.x * 2.0f;
        float leftWidth = canvas.x * 0.4f - gap;
        auto leftPos = tl;
        auto leftSize = ImVec2(leftWidth, canvas.y);
        auto rightPos = ImVec2(tl.x + leftWidth + gap, tl.y);
        auto rightSize = ImVec2(-1, canvas.y);
        auto winPos = GetWindowPos();
        auto sepPosA =
            ImVec2(winPos.x + tl.x + leftWidth + (gap / 2), winPos.y + tl.y);
        auto sepPosB = ImVec2(sepPosA.x, winPos.y + tl.y + canvas.y);

        // ── Left panel: page list ──
        SetCursorPos(leftPos);
        BeginChild("##wiki-left",
            leftSize,
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_None);

        if (!pages_result) {
            TextDisabled("(wiki unavailable)");
        } else if (pages_result->empty()) {
            TextDisabled("(no pages)");
        } else {
            for (const auto& title : *pages_result) {
                bool is_selected = (title == selected_page);
                if (Selectable(title.c_str(), is_selected)) {
                    selected_page = title;
                }
                if (is_selected) {
                    SetItemDefaultFocus();
                }
            }
        }

        EndChild();

        // ── Separator line ──
        GetWindowDrawList()->AddLine(sepPosA,
            sepPosB,
            GetColorU32(ImGuiCol_Separator),
            GetStyle().ChildBorderSize);

        // ── Right panel: page content ──
        SetCursorPos(rightPos);
        BeginChild("##wiki-right",
            rightSize,
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_None);

        if (selected_page.empty()) {
            TextDisabled("Select a page from the left panel");
        } else {
            auto content_result = wiki.read_page(selected_page);
            if (!content_result) {
                TextColored(ImColor(IM_COL32(255, 100, 100, 255)), "%s",
                    content_result.error().c_str());
            } else {
                PushFont(mono_font);
                render_content(*content_result);
                PopFont();
            }
        }

        EndChild();
    }
}
