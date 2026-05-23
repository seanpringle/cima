#include "gui_chat.h"
#include "gui_markdown.h"
#include "plan.h"
#include "tools.h"
#include <cassert>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

// ── InputText callback: track cursor position for insert-at-cursor ──────
static int InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* pos = static_cast<int*>(data->UserData);
    *pos = data->CursorPos;
    return 0;
}

// ── History tab (left panel): selectable list of past inputs ──
void render_history_tab(PrimaryAgent& tab) {
    auto& ui = tab.ui_state;
    auto& history = ui.input_history;

    if (!history.empty()) {
        if (Button("Clear History")) {
            ui.input_history.clear();
        }
        Separator();
    }

    if (history.empty()) {
        TextDisabled("No history yet");
        return;
    }

    // Most recent first
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (Selectable(it->c_str())) {
            ui.input_buffer.assign(it->begin(), it->end());
            ui.cursor_pos = static_cast<int>(it->size());
        }
    }
}

// ── Tag expansion for !snippet-name references ──
// Expands !snippetname to the full snippet content.  Non-matching tags are left as-is.
// Session snippets take precedence over config snippets when names collide.
static std::string expand_tags(
    std::string input, const std::map<std::string, std::string>& session_snippets) {
    std::string result;
    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '!') {
            size_t start = i + 1;
            size_t end = start;
            while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end])))
                end++;
            std::string name = input.substr(start, end - start);
            if (!name.empty()) {
                // Session snippets take precedence
                auto it = session_snippets.find(name);
                if (it != session_snippets.end()) {
                    result += it->second;
                    i = end;
                    continue;
                }
                // Fall back to config/file-based snippets
                it = cfg.snippets.find(name);
                if (it != cfg.snippets.end()) {
                    result += it->second;
                    i = end;
                    continue;
                }
            }
        }
        result += input[i];
        i++;
    }
    return result;
}

void render_subagent_tab(SubAgent& tab) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

    // ── Provider combo ──
    {
        string label = tab.provider_name.empty() ? cfg.providers[0].name : tab.provider_name;
        if (BeginCombo("Provider", label.c_str())) {
            for (const auto& p : cfg.providers) {
                bool is_selected = (p.name == tab.provider_name);
                if (Selectable(p.name.c_str(), is_selected)) {
                    if (p.name != tab.provider_name) {
                        session.set_provider(p);
                        tab.provider_name = p.name;
                        tab.model_name = p.model;
                        tab.reasoning_effort = p.reasoning_effort;
                        tab.ui_state.models_validated = false;
                    }
                }
                if (is_selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
    }

    // ── Model combo (or manual text input if fetch failed) ──

    auto& cache_entry = g_provider_models[tab.provider_name];

    if (!cache_entry.fetched) {
        PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        Text("Model:");
        SameLine();
        TextUnformatted("Loading models...");
        PopStyleColor();
    } else if (!cache_entry.error.empty() || cache_entry.models.empty()) {
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        Text("Model:");
        SameLine();
        if (!cache_entry.error.empty()) {
            TextUnformatted(cache_entry.error.c_str());
        } else {
            TextDisabled("(no models returned)");
        }
        PopStyleColor();
        char buf[256];
        strncpy(buf, session.model().c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (InputText("##model-manual", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string new_model(buf);
            if (!new_model.empty()) {
                session.set_model(new_model);
                tab.model_name = new_model;
            }
        }
    } else {
        if (BeginCombo("Model", session.model().c_str())) {
            for (const auto& m : cache_entry.models) {
                bool is_selected = (m == session.model());
                if (Selectable(m.c_str(), is_selected)) {
                    session.set_model(m);
                    tab.model_name = m;
                }
                if (is_selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
    }

    tab.validate_current_model();

    // ── Reasoning effort combo ──
    {
        std::string re =
            tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

        std::vector<std::string> efforts;
        for (const auto& p : cfg.providers) {
            if (p.name == tab.provider_name) {
                efforts = p.reasoning_efforts;
                break;
            }
        }

        if (BeginCombo("Reasoning Effort", re.empty() ? "(unset)" : re.c_str())) {
            if (Selectable("(unset)", re.empty())) {
                tab.reasoning_effort.clear();
                session.set_reasoning_effort("");
            }
            for (const auto& e : efforts) {
                bool is_selected = (e == re);
                if (Selectable(e.c_str(), is_selected)) {
                    tab.reasoning_effort = e;
                    session.set_reasoning_effort(e);
                }
                if (is_selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
}

void render_subagent_chat(SubAgent& tab) {
    auto& ui = tab.ui_state;
    auto& chat = *tab.chat_state;

    // Drain any pending streaming output first
    tab.drain_pending();

    // Check if subagent chat finished
    {
        auto finished = tab.check_finished();
        if (finished && !*finished) {
            ui.push_entry(EntryType::Content, "Error: " + finished->error(), false);
        }
    }

    // Render entries (same style as main chat in render_chat_ui)
    BeginChild("##subagent-chat", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None);
    for (size_t i = 0; i < ui.entries.size(); i++) {
        auto& entry = ui.entries[i];

        if (entry.type == EntryType::Content && !entry.text.size())
            continue;

        PushID(("sa-entry-" + std::to_string(i)).c_str());
        render_display_entry(ui, entry, i, "Primary");
        PopID();
    }

    NewLine();

    if (chat.running) {
        SameLine();
        TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Subagent is thinking...");
    }

    // auto-scroll
    float scroll_y = GetScrollY();
    float scroll_max = GetScrollMaxY();
    if (scroll_y >= scroll_max - GetFrameHeightWithSpacing()) {
        SetScrollHereY(1.0f);
    }

    EndChild();
}

void render_chat_ui(PrimaryAgent& tab, bool& done) {
    auto& ui = tab.ui_state;
    auto& chat = *tab.chat_state;
    auto& session = *tab.session;

    // ── check if chat finished (before drain, so the drain catches any last items) ──
    bool stream_ended = false;
    Result<ChatResult> result = std::unexpected(string("unknown error"));
    {
        auto finished = tab.check_finished();
        if (finished) {
            result = std::move(*finished);
            stream_ended = true;
        }
    }

    // ── drain pending output (includes any items that arrived after last frame's drain) ──
    tab.drain_pending();

    // ── Handle compact request ──
    tab.poll_compact();

    // ── Handle clear request ──
    tab.poll_clear();

    // ── finalize streaming entry now that all pending data is incorporated ──
    if (stream_ended) {
        ui.finalize_streaming_entry();
        if (!result) {
            ui.push_entry(EntryType::Content, "Error: " + result.error(), false);
        }
    }

    // ── main content ──
    float input_height = GetFrameHeightWithSpacing() * 6 + 8;

    BeginChild("##chat", ImVec2(0, -input_height), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    size_t i = 0;

    if (ui.entries.size() > 100) {
        i = ui.entries.size() - 100;
        TextWrapped("%d old entries", int(i));
        Separator();
    }

    for (; i < ui.entries.size(); i++) {
        auto& entry = ui.entries[i];

        if (entry.type == EntryType::Content && !entry.text.size())
            continue;

        PushID(string("entry-" + std::to_string(i)).c_str());
        render_display_entry(ui, entry, i, "You");
        PopID();
    }

    NewLine();

    // auto-scroll
    float scroll_y = GetScrollY();
    float scroll_max = GetScrollMaxY();
    if (scroll_y >= scroll_max - GetFrameHeightWithSpacing()) {
        SetScrollHereY(1.0f);
    }

    EndChild();

    auto insert_text_at_cursor = [&](string_view text) {
        auto& buf = ui.input_buffer;
        int pos = ui.cursor_pos;
        if (pos < 0 || (size_t)pos > strlen(buf.data()))
            pos = (int)strlen(buf.data()); // append
        // Make room and insert into buffer
        size_t room = buf.size() - strlen(buf.data()) - 1;
        size_t insert_len = text.size();
        if (insert_len > room)
            insert_len = room;
        memmove(buf.data() + pos + insert_len, buf.data() + pos, strlen(buf.data()) - pos + 1);
        memcpy(buf.data() + pos, text.data(), insert_len);
        ui.cursor_pos = pos + (int)insert_len;
    };

    SetNextItemWidth(GetContentRegionAvail().x);

    PushFont(mono_font);

    auto trimWhite = [](string_view cur) -> string_view {
        while (cur.size() && isspace(cur.front()))
            cur.remove_prefix(1);
        while (cur.size() && isspace(cur.back()))
            cur.remove_suffix(1);
        return cur;
    };

    auto& buffer = ui.input_buffer;
    auto& history = ui.input_history;

    // Ensure adequate capacity once; avoid per-frame resize
    constexpr size_t kInputBufferSize = 1024 * 1024;
    if (buffer.size() < kInputBufferSize) {
        buffer.resize(kInputBufferSize, 0);
    }

    if (!chat.running && IsWindowAppearing()) {
        SetKeyboardFocusHere();
    }

    uint32_t inputFlags = ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap |
        ImGuiInputTextFlags_CallbackAlways;

    ImVec2 inputSize(0, GetContentRegionAvail().y - GetFrameHeight());

    if (InputTextMultiline("##input",
            buffer.data(),
            buffer.size(),
            inputSize,
            inputFlags,
            InputTextCallback,
            &ui.cursor_pos) &&
        !chat.running && !tab.chat_state->compact_running) {
        string input(trimWhite(buffer.data()));
        if (input.size()) {
            // Push to UI with tags visible (user sees @Page / !Snippet)
            ui.push_entry(EntryType::UserText, input, false);
            // Expand !snippet-name tags before sending to the agent
            // Session snippets take precedence over config snippets.
            string expanded = expand_tags(input, tab.session_data.snippets);
            tab.start_chat(expanded);
            for (auto it = history.begin(); it != history.end();
                it = *it == input ? history.erase(it) : ++it)
                ;
            history.push_back(input);
            buffer.front() = 0;
        }
    }

    PopFont();

    // ── Footer: status bar ──
    {
        auto& session = *tab.session;

        // State
        string stateInfo;
        ImU32 stateColor;
        if (tab.chat_state->running) {
            stateInfo = "running";
            stateColor = IM_COL32(100, 255, 100, 255);
        } else if (tab.chat_state->compact_running) {
            stateInfo = "compacting";
            stateColor = IM_COL32(255, 208, 0, 255);
        } else {
            stateInfo = "idle";
            stateColor = IM_COL32(180, 180, 180, 255);
        }

        // Context usage percentage (for token colour)
        int context_pct = session.context_usage_percent();

        ImU32 tokenColor;
        if (context_pct >= 90)
            tokenColor = IM_COL32(255, 68, 68, 255);
        else if (context_pct >= 60)
            tokenColor = IM_COL32(255, 208, 0, 255);
        else
            tokenColor = IM_COL32(180, 180, 180, 255);

        // Token count — use API-reported usage, fall back to conversation estimate
        // (the latter is available immediately after restart, before any prompt completes)
        int tokens = session.last_usage().total_tokens;
        if (tokens == 0) {
            tokens = static_cast<int>(session.conversation().estimate_total_tokens());
        }
        string tokenInfo = std::to_string(tokens) + " tokens";

        // Git branch (safe_dir locked to cwd, fetched each render loop)
        {
            auto branch_result = get_current_git_branch(session.safe_dir());
            if (branch_result) {
                tab.git_branch = std::move(*branch_result);
            } else {
                tab.git_branch.clear();
            }
        }
        string branchInfo = tab.git_branch;

        string sep = " :: ";

        // Right-aligned footer line
        auto stateSize = CalcTextSize(stateInfo.c_str());
        auto branchSize = CalcTextSize(branchInfo.c_str());
        auto tokenSize = CalcTextSize(tokenInfo.c_str());
        auto sepSize = CalcTextSize(sep.c_str());

        // Lay out right-aligned: [branch] :: [tokens] :: [state]
        ImVec2 pos = GetCursorScreenPos() + ImVec2(GetContentRegionAvail().x, 0);

        pos = pos - ImVec2(branchSize.x, 0);
        GetForegroundDrawList()->AddText(
            pos, ImColor(IM_COL32(255, 180, 50, 255)), branchInfo.c_str());

        pos = pos - ImVec2(sepSize.x, 0);
        GetForegroundDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());

        pos = pos - ImVec2(tokenSize.x, 0);
        GetForegroundDrawList()->AddText(pos, tokenColor, tokenInfo.c_str());

        pos = pos - ImVec2(sepSize.x, 0);
        GetForegroundDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());

        pos = pos - ImVec2(stateSize.x, 0);
        GetForegroundDrawList()->AddText(pos, stateColor, stateInfo.c_str());
    }

    PushStyleColor(ImGuiCol_Text, GetColorU32(ImGuiCol_TextDisabled));
    TextUnformatted(tab.session->safe_dir().c_str());
    PopStyleColor();
}
