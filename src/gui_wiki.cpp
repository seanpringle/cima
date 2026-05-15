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
    // ── Sub-tab bar ──
    enum SubTab { SubPages, SubSnippets };
    static SubTab current_sub = SubPages;

    if (BeginTabBar("##wiki-sub")) {
        if (BeginTabItem("Pages")) {
            current_sub = SubPages;
            EndTabItem();
        }
        if (BeginTabItem("Snippets")) {
            current_sub = SubSnippets;
            EndTabItem();
        }
        EndTabBar();
    }

    // ── Fetch data ──
    static std::string selected_page;
    static std::string selected_snippet;
    static char snippet_name_buf[1024] = {0};
    static char snippet_content_buf[65536] = {0};

    if (current_sub == SubPages) {
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
                        PushFont(mono_font);
                        render_content(*content_result);
                        PopFont();
                    }
                }
            }
        );
    } else {
        // ── Snippets sub-tab ──
        auto snippets_result = wiki.list_snippets();

        if (snippets_result) {
            bool found = false;
            for (const auto& [name, _] : *snippets_result) {
                if (name == selected_snippet) { found = true; break; }
            }
            if (!found) {
                selected_snippet.clear();
                snippet_name_buf[0] = '\0';
                snippet_content_buf[0] = '\0';
            }
        }

        layout_left_right(wiki, mono_font,
            [&] {
                // ── Left: snippet list ──
                if (!snippets_result) {
                    TextDisabled("(wiki unavailable)");
                } else {
                    for (const auto& [name, _] : *snippets_result) {
                        bool is_sel = (name == selected_snippet);
                        if (Selectable(name.c_str(), is_sel)) {
                            selected_snippet = name;
                            // Load into edit buffers
                            size_t n = name.copy(snippet_name_buf, sizeof(snippet_name_buf) - 1);
                            snippet_name_buf[n] = '\0';
                            // Find content
                            for (const auto& [n2, c] : *snippets_result) {
                                if (n2 == name) {
                                    size_t n3 = c.copy(snippet_content_buf, sizeof(snippet_content_buf) - 1);
                                    snippet_content_buf[n3] = '\0';
                                    break;
                                }
                            }
                        }
                        if (is_sel) SetItemDefaultFocus();
                    }

                    // ── Add button ──
                    Separator();
                    if (SmallButton("+ Add")) {
                        // Find a unique placeholder name
                        std::string base = "snippet";
                        int n = 1;
                        bool unique = false;
                        while (!unique) {
                            std::string candidate = base + std::to_string(n);
                            unique = true;
                            for (const auto& [name, _] : *snippets_result) {
                                if (name == candidate) { unique = false; break; }
                            }
                            if (unique) {
                                wiki.write_snippet(candidate, "");
                                selected_snippet = candidate;
                                candidate.copy(snippet_name_buf, sizeof(snippet_name_buf) - 1);
                                snippet_name_buf[candidate.size()] = '\0';
                                snippet_content_buf[0] = '\0';
                            }
                            n++;
                        }
                    }
                }
            },
            [&] {
                // ── Right: snippet editor ──
                if (selected_snippet.empty()) {
                    TextDisabled("Select a snippet from the left panel");
                } else {
                    // Name (plain text buffer, no auto-save)
                    PushID("snippet-name");
                    Text("Name:");
                    SameLine();
                    InputText("##name", snippet_name_buf, sizeof(snippet_name_buf));

                    // Save button (blue-green, explicit persist)
                    SameLine();
                    PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.35f, 1.0f));
                    PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.55f, 0.45f, 1.0f));
                    PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.35f, 0.25f, 1.0f));
                    if (SmallButton("Save")) {
                        std::string new_name(snippet_name_buf);
                        if (!new_name.empty()) {
                            if (new_name != selected_snippet) {
                                // Rename: write under new name, delete old
                                wiki.write_snippet(new_name, std::string(snippet_content_buf));
                                wiki.delete_snippet(selected_snippet);
                                selected_snippet = new_name;
                            } else {
                                // Same name: just update content
                                wiki.write_snippet(selected_snippet, std::string(snippet_content_buf));
                            }
                        }
                    }
                    PopStyleColor(3);

                    // Delete button (red, destructive)
                    SameLine();
                    PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.15f, 0.15f, 1.0f));
                    PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                    PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
                    if (SmallButton("Delete")) {
                        wiki.delete_snippet(selected_snippet);
                        selected_snippet.clear();
                        snippet_name_buf[0] = '\0';
                        snippet_content_buf[0] = '\0';
                    }
                    PopStyleColor(3);

                    PopID();

                    // Content (multi-line, saved only via Save button)
                    PushID("snippet-content");
                    Text("Content:");
                    InputTextMultiline("##content", snippet_content_buf, sizeof(snippet_content_buf),
                        ImVec2(-1, GetContentRegionAvail().y - 4),
                        ImGuiInputTextFlags_AllowTabInput);
                    PopID();
                }
            }
        );
    }
}
