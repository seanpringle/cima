#include "gui_chat.h"
#include "tools.h"
#include <cassert>
#include <iostream>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <md4c.h>
#include <string>
#include <future>
#include <thread>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

namespace {

void text_unformatted_ellipsis(const string& text) {
    auto canvas = GetContentRegionAvail();
    auto size = CalcTextSize(text.c_str());
    if (size.x < canvas.x) {
        TextUnformatted(text.c_str());
        return;
    }
    auto glyph = CalcTextSize("_");
    int cols = std::max(0, std::min(int(text.size()), int(canvas.x / glyph.x) - 4));
    stringstream ss;
    ss << string_view(text.data(), cols) << "...";
    TextUnformatted(ss.str().c_str());
}

void text_unformatted_inline_wrap(const string& text) {
    auto blit = [&](string_view chunk) {
        auto size = CalcTextSize(chunk.data(), chunk.data() + chunk.size());
        if (!(GetContentRegionAvail().x > size.x))
            NewLine();
        auto pos = GetCursorPos();
        TextUnformatted(chunk.data(), chunk.data() + chunk.size());
        SetCursorPos(ImVec2(pos.x + size.x, pos.y));
    };

    string_view cur(text);

    while (cur.size()) {
        if (std::isspace(cur.front())) {
            blit(string_view(cur.data(), 1));
            cur.remove_prefix(1);
            continue;
        }
        auto left = cur;
        while (cur.size() && !std::isspace(cur.front()))
            cur.remove_prefix(1);
        blit(string_view(left.data(), left.size() - cur.size()));
    }
}

struct RenderCtx {
    int style_depth = 0;
    int tables = 0;
    // code block rendering
    bool in_code_block = false;
    string code_buf;
    ImVec2 code_start;
    ImDrawListSplitter code_splitter;
    int list_levels = 0;

    vector<float> indents;

    void indent(float w) {
        Indent(w);
        indents.push_back(w);
    }

    void unindent() {
        if (indents.size()) {
            Unindent(indents.back());
            indents.pop_back();
        }
    }

    void newline(MD_BLOCKTYPE /*type*/) { NewLine(); }

    void newline(MD_TEXTTYPE /*type*/) { NewLine(); }
};

static int enter_block_cb(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& ctx = *static_cast<RenderCtx*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_P:
        PushTextWrapPos(0);
        break;
    case MD_BLOCK_H: {
        auto* h = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        PushTextWrapPos(0);
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        ctx.style_depth++;
        for (auto i = 0; i < h->level; i++) {
            text_unformatted_inline_wrap("#");
        }
        text_unformatted_inline_wrap(" ");
        break;
    }
    case MD_BLOCK_CODE: {
        ctx.in_code_block = true;
        ctx.code_buf.clear();
        ctx.code_start = GetCursorScreenPos();
        auto* dl = GetWindowDrawList();
        ctx.code_splitter.Split(dl, 2);
        ctx.code_splitter.SetCurrentChannel(dl, 1);
        ctx.indent(GetStyle().IndentSpacing);
        ctx.newline(type);
        break;
    }
    case MD_BLOCK_HR:
        Separator();
        break;
    case MD_BLOCK_UL:
        if (ctx.list_levels)
            ctx.newline(type);
        ctx.indent(GetStyle().IndentSpacing);
        ctx.list_levels++;
        break;
    case MD_BLOCK_OL:
        if (ctx.list_levels)
            ctx.newline(type);
        ctx.indent(GetStyle().IndentSpacing);
        ctx.list_levels++;
        break;
    case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        auto before = GetCursorPos();
        if (li->is_task) {
            string mark = (li->task_mark == 'x' || li->task_mark == 'X') ? "[x] " : "[ ] ";
            text_unformatted_inline_wrap(mark);
        } else {
            Bullet();
        }
        auto after = GetCursorPos();
        SetCursorPos(before);
        ctx.indent(after.x - before.x);
        break;
    }
    case MD_BLOCK_TABLE: {
        auto* table = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        string tid = "##tbl" + std::to_string(++ctx.tables);
        BeginTable(tid.c_str(), table->col_count, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
        for (unsigned i = 0; i < table->col_count; i++) {
            string cid = "##c" + std::to_string(i);
            ImGuiTableColumnFlags flags = (i == table->col_count - 1)
                ? ImGuiTableColumnFlags_WidthStretch
                : ImGuiTableColumnFlags_None;
            TableSetupColumn(cid.c_str(), flags);
        }
        break;
    }
    case MD_BLOCK_THEAD:
        break;
    case MD_BLOCK_TBODY:
        break;
    case MD_BLOCK_TR:
        TableNextRow();
        break;
    case MD_BLOCK_TH: {
        TableNextColumn();
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        ctx.style_depth++;
        break;
    }
    case MD_BLOCK_TD: {
        TableNextColumn();
        break;
    }
    case MD_BLOCK_QUOTE:
        ctx.indent(GetStyle().IndentSpacing);
        PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));
        ctx.style_depth++;
        break;
    case MD_BLOCK_HTML:
        break;
    }
    return 0;
}

static int leave_block_cb(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& ctx = *static_cast<RenderCtx*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC:
        break;
    case MD_BLOCK_P:
        PopTextWrapPos();
        ctx.newline(type);
        if (ctx.list_levels == 0)
            ctx.newline(type);
        break;
    case MD_BLOCK_H:
        PopStyleColor();
        ctx.style_depth--;
        PopTextWrapPos();
        ctx.newline(type);
        ctx.newline(type);
        break;
    case MD_BLOCK_CODE: {
        // render buffered code text
        if (!ctx.code_buf.empty()) {
            size_t pos = 0;
            while (pos < ctx.code_buf.size()) {
                size_t nl = ctx.code_buf.find('\n', pos);
                string line = (nl == string::npos) ? ctx.code_buf.substr(pos)
                                                   : ctx.code_buf.substr(pos, nl - pos);
                TextUnformatted(line.c_str());
                if (nl == string::npos)
                    break;
                pos = nl + 1;
                SetCursorScreenPos(
                    ImVec2(ctx.code_start.x + GetStyle().IndentSpacing, GetCursorScreenPos().y));
            }
        }
        ctx.newline(type);
        ImVec2 br(GetCursorScreenPos().x + GetContentRegionAvail().x, GetCursorScreenPos().y);
        auto* dl = GetWindowDrawList();
        ctx.code_splitter.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(ctx.code_start, br, GetColorU32(ImGuiCol_TableRowBgAlt));
        ctx.code_splitter.Merge(dl);
        ctx.in_code_block = false;
        ctx.unindent();
        ctx.newline(type);
        break;
    }
    case MD_BLOCK_HR:
        ctx.newline(type);
        break;
    case MD_BLOCK_UL:
        ctx.unindent();
        if (ctx.list_levels < 2)
            ctx.newline(type);
        ctx.list_levels--;
        break;
    case MD_BLOCK_OL:
        ctx.unindent();
        if (ctx.list_levels < 2)
            ctx.newline(type);
        ctx.list_levels--;
        break;
    case MD_BLOCK_LI:
        ctx.newline(type);
        ctx.unindent();
        break;
    case MD_BLOCK_TABLE:
        EndTable();
        ctx.newline(type);
        break;
    case MD_BLOCK_THEAD:
        break;
    case MD_BLOCK_TBODY:
        break;
    case MD_BLOCK_TR:
        break;
    case MD_BLOCK_TH:
        PopStyleColor();
        ctx.style_depth--;
        break;
    case MD_BLOCK_TD:
        break;
    case MD_BLOCK_QUOTE:
        PopStyleColor();
        ctx.style_depth--;
        ctx.unindent();
        ctx.newline(type);
        break;
    case MD_BLOCK_HTML:
        break;
    }
    return 0;
}

static int enter_span_cb(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& ctx = *static_cast<RenderCtx*>(userdata);
    switch (type) {
    case MD_SPAN_STRONG:
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 200, 255));
        ctx.style_depth++;
        break;
    case MD_SPAN_CODE:
        PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 255, 255));
        ctx.style_depth++;
        break;
    case MD_SPAN_A:
        PushStyleColor(ImGuiCol_Text, IM_COL32(100, 150, 255, 255));
        ctx.style_depth++;
        break;
    case MD_SPAN_DEL:
        PushStyleColor(ImGuiCol_Text, IM_COL32(140, 140, 140, 255));
        ctx.style_depth++;
        break;
    case MD_SPAN_EM:
        break;
    case MD_SPAN_U:
        break;
    case MD_SPAN_IMG:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
    case MD_SPAN_WIKILINK:
        break;
    }
    (void)detail;
    return 0;
}

static int leave_span_cb(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& ctx = *static_cast<RenderCtx*>(userdata);
    switch (type) {
    case MD_SPAN_STRONG:
    case MD_SPAN_CODE:
    case MD_SPAN_A:
    case MD_SPAN_DEL:
        PopStyleColor();
        ctx.style_depth--;
        break;
    default:
        break;
    }
    (void)detail;
    return 0;
}

static int text_cb(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& ctx = *static_cast<RenderCtx*>(userdata);
    switch (type) {
    case MD_TEXT_NORMAL:
        text_unformatted_inline_wrap(string(text, size));
        break;
    case MD_TEXT_CODE:
        if (ctx.in_code_block) {
            ctx.code_buf.append(text, size);
        } else {
            text_unformatted_inline_wrap(string(text, size));
        }
        break;
    case MD_TEXT_BR:
        ctx.newline(type);
        break;
    case MD_TEXT_SOFTBR:
        text_unformatted_inline_wrap(" ");
        break;
    case MD_TEXT_ENTITY:
        text_unformatted_inline_wrap(string(text, size));
        break;
    case MD_TEXT_NULLCHAR:
        text_unformatted_inline_wrap("\xef\xbf\xbd");
        break;
    case MD_TEXT_HTML:
    case MD_TEXT_LATEXMATH:
        break;
    }
    return 0;
}

} // anonymous namespace

void render_content(const string& text) {
    string_view trim(text);
    while (trim.size() && std::isspace(trim.back()))
        trim.remove_suffix(1);

    string clean(trim);
    clean.erase(
        std::remove_if(clean.begin(), clean.end(), [](auto c) { return c == '\r' || c == '\0'; }),
        clean.end());
    if (clean.empty())
        return;

    RenderCtx ctx;

    MD_PARSER parser = {};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_COLLAPSEWHITESPACE;
    parser.enter_block = enter_block_cb;
    parser.leave_block = leave_block_cb;
    parser.enter_span = enter_span_cb;
    parser.leave_span = leave_span_cb;
    parser.text = text_cb;

    md_parse(clean.data(), (MD_SIZE)clean.size(), &parser, &ctx);

    while (ctx.style_depth > 0) {
        PopStyleColor();
        ctx.style_depth--;
    }
}

static void start_chat(AsyncChatState& chat, ChatSession& session, string input) {
    chat.running = true;
    *chat.cancelled = false;
    chat.future = std::async(std::launch::async,
        [&session, input = std::move(input)]() { return session.run_once(input); });
}

static void push_entry(ChatUIState& ui, EntryType type, const string& text, bool streaming) {
    ui.entries.push_back({type, text, streaming, ui.next_seq++});
}

void drain_pending(ChatUIState& ui, AsyncChatState& chat) {
    std::lock_guard<std::mutex> lock(chat.mutex);
    for (auto& [pending_text, type] : chat.pending) {
        if (type == OutputType::ToolInvocation) {
            if (!ui.entries.empty() && ui.entries.back().is_streaming) {
                ui.entries.back().is_streaming = false;
            }
            push_entry(ui, EntryType::ToolCall, pending_text, false);
        } else {
            auto entry_type =
                (type == OutputType::Reasoning) ? EntryType::Reasoning : EntryType::Content;
            if (!ui.entries.empty() && ui.entries.back().is_streaming &&
                ui.entries.back().type == entry_type) {
                ui.entries.back().text += pending_text;
            } else {
                if (!ui.entries.empty() && ui.entries.back().is_streaming) {
                    ui.entries.back().is_streaming = false;
                }
                push_entry(ui, entry_type, pending_text, true);
            }
        }
    }
    chat.pending.clear();
}

void render_chat_controls(TabInfo& tab) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

    // ── Fetch models on first render ──
    if (!ui.models_loaded) {
        ui.models_loaded = true;
        // Fire off an async fetch so the UI stays responsive.
        // Use a packaged_task so we can track completion via the future
        // and wait for it before destroying the tab (prevents use-after-free).
        std::packaged_task<void()> task([&ui, &session]() {
            auto result = session.client_for_models().fetch_models();
            if (result) {
                ui.available_models = std::move(*result);
            } else {
                ui.models_error = std::move(result.error());
            }
            ui.models_fetched->store(true, std::memory_order_release);
        });
        ui.models_future = task.get_future();
        std::thread(std::move(task)).detach();
    }

    // Validate current model selection on the render thread (once)
    // so we can safely access tab.session and tab.title without races.
    if (!ui.models_validated) {
        if (ui.models_fetched->load(std::memory_order_acquire)) {
            ui.models_validated = true;
            if (!ui.available_models.empty()) {
                const auto& current = session.model();
                bool found = std::any_of(ui.available_models.begin(), ui.available_models.end(),
                    [&current](const std::string& m) { return m == current; });
                if (!found) {
                    session.set_model(ui.available_models.front());
                    tab.title = ui.available_models.front();
                }
            }
        }
    }

    if (BeginMenuBar()) {
        if (BeginMenu("Model")) {
            // Show a loading indicator if models haven't arrived yet
            // Acquire-load synchronises with the release-store in the
            // model-fetch thread, making available_models/models_error visible.
            if (!ui.models_fetched->load(std::memory_order_acquire)) {
                TextDisabled("Loading models...");
            } else if (!ui.models_error.empty()) {
                TextColored(
                    ImColor(IM_COL32(255, 100, 100, 255)), "Error: %s", ui.models_error.c_str());
            } else if (ui.available_models.empty()) {
                TextDisabled("(no models returned)");
            } else {
                for (const auto& m : ui.available_models) {
                    bool is_selected = (m == session.model());
                    if (MenuItem(m.c_str(), nullptr, is_selected)) {
                        session.set_model(m);
                        // Update the tab title to reflect the new model
                        tab.title = m;
                    }
                }
            }
            EndMenu();
        }
        if (BeginMenu("Debug")) {
            Checkbox("Raw", &ui.show_raw);
            EndMenu();
        }
        EndMenuBar();
    }

    string tokenInfo = std::to_string(session.last_usage().total_tokens) + " tokens";

    // Refresh workspace path from the session (may change via worktree tools)
    auto current_safe_dir = session.safe_dir();
    if (current_safe_dir != tab.workspace_path) {
        tab.workspace_path = current_safe_dir;
        auto branch_result = get_current_git_branch(current_safe_dir);
        if (branch_result) {
            tab.git_branch = std::move(*branch_result);
        } else {
            tab.git_branch.clear();
        }
    }

    string branchInfo = tab.git_branch;

    string sep = " :: ";

    auto tokenInfoSize = CalcTextSize(tokenInfo.c_str());
    auto branchInfoSize = CalcTextSize(branchInfo.c_str());
    auto sepSize = CalcTextSize(sep.c_str());

    auto branchPos = ImVec2(GetContentRegionMax().x - branchInfoSize.x - GetStyle().WindowPadding.x,
        GetFrameHeight() / 2 - branchInfoSize.y / 2);
    auto sepPos = ImVec2(branchPos.x - sepSize.x, GetFrameHeight() / 2 - sepSize.y / 2);
    auto tokenPos = ImVec2(sepPos.x - tokenInfoSize.x, GetFrameHeight() / 2 - tokenInfoSize.y / 2);

    GetForegroundDrawList()->AddText(
        GetWindowPos() + tokenPos, ImColor(IM_COL32(180, 180, 180, 255)), tokenInfo.c_str());
    GetForegroundDrawList()->AddText(
        GetWindowPos() + sepPos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());
    GetForegroundDrawList()->AddText(
        GetWindowPos() + branchPos, ImColor(IM_COL32(255, 180, 50, 255)), branchInfo.c_str());

    SetCursorPos(ImVec2(0, GetFrameHeightWithSpacing()));
}

void render_chat_ui(TabInfo& tab, bool& done) {
    auto& ui = tab.ui_state;
    auto& chat = *tab.chat_state;
    auto& session = *tab.session;

    // ── check if chat finished (before drain, so the drain catches any last items) ──
    bool stream_ended = false;
    Result<ChatResult> result = std::unexpected(string("unknown error"));
    if (chat.running && chat.future.valid() &&
        chat.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            result = chat.future.get();
        } catch (const std::exception& e) {
            result = std::unexpected(string(e.what()));
        }
        chat.running = false;
        stream_ended = true;
    }

    // ── drain pending output (includes any items that arrived after last frame's drain) ──
    drain_pending(ui, chat);

    // ── finalize streaming entry now that all pending data is incorporated ──
    if (stream_ended) {
        if (!ui.entries.empty() && ui.entries.back().is_streaming) {
            ui.entries.back().is_streaming = false;
        }
        if (!result) {
            push_entry(ui, EntryType::Content, "Error: " + result.error(), false);
        }
    }

    // ── main content ──
    float input_height = GetFrameHeightWithSpacing() * 6 + 8;

    PushFont(ui.mono_font);

    BeginChild("##chat", ImVec2(0, -input_height), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    size_t i = 0;

    if (ui.entries.size() > 30) {
        i = ui.entries.size() - 30;
        TextWrapped("%d old entries", int(i));
        Separator();
    }

    for (; i < ui.entries.size(); i++) {
        auto& entry = ui.entries[i];

        if (entry.type == EntryType::Content && !entry.text.size())
            continue;

        PushID(string("entry-" + std::to_string(i)).c_str());

        stringstream ss;

        if (ui.show_raw) {
            // ── Raw text mode ──
            const char* prefix = "";
            switch (entry.type) {
            case EntryType::UserText:
                prefix = "[You] ";
                break;
            case EntryType::Reasoning:
                prefix = "[Reasoning] ";
                break;
            case EntryType::Content:
                prefix = "[Assistant] ";
                break;
            case EntryType::ToolCall:
                prefix = "[Tool] ";
                break;
            }
            PushTextWrapPos(0);
            ss << prefix << entry.text;
            TextUnformatted(ss.str().c_str());
            PopTextWrapPos();
        } else {
            // ── Pretty markdown mode ──
            switch (entry.type) {
            case EntryType::UserText:
                PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
                PushTextWrapPos(0);
                ss << "You: " << entry.text;
                TextUnformatted(ss.str().c_str());
                NewLine();
                PopTextWrapPos();
                PopStyleColor();
                break;
            case EntryType::Reasoning:
                PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
                render_content("Thinking: " + entry.text);
                PopStyleColor();
                break;
            case EntryType::Content:
                PushStyleColor(ImGuiCol_Text, GetColorU32(ImGuiCol_Text));
                render_content(entry.text);
                PopStyleColor();
                break;
            case EntryType::ToolCall:
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 165, 0, 255));
                PushTextWrapPos(0);
                text_unformatted_ellipsis(entry.text);
                for (; i + 1 < ui.entries.size() && ui.entries[i + 1].type == EntryType::ToolCall;
                    i++) {
                    text_unformatted_ellipsis(ui.entries[i + 1].text);
                }
                NewLine();
                PopTextWrapPos();
                PopStyleColor();
                break;
            }
        }

        PopID();
    }

    NewLine();

    // auto-scroll
    float scroll_y = GetScrollY();
    float scroll_max = GetScrollMaxY();
    if (scroll_y >= scroll_max - 10.0f)
        ui.auto_scroll = true;
    else
        ui.auto_scroll = false;
    if (ui.auto_scroll)
        SetScrollHereY(1.0f);

    EndChild();

    PopFont();

    // ── input area ──
    Separator();

    SetNextItemWidth(GetContentRegionAvail().x);

    PushFont(ui.mono_font);

    uint32_t inputFlags = ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap;

    ImVec2 inputSize(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4));

    auto trimWhite = [](string_view cur) -> string_view {
        while (cur.size() && isspace(cur.front())) cur.remove_prefix(1);
        while (cur.size() && isspace(cur.back())) cur.remove_suffix(1);
        return cur;
    };

    auto& buffer = ui.input_buffer;
    auto& history = ui.input_history;

    if (IsKeyDown(ImGuiKey_LeftCtrl) && IsKeyReleased(ImGuiKey_UpArrow) && history.size() > 0) {
        history.emplace_back() = std::move(history.front());
        history.pop_front();
        buffer = {history.back().begin(), history.back().end()};
        buffer.resize(buffer.size()+10,0);
        SetKeyboardFocusHere();
    }

    if (IsKeyDown(ImGuiKey_LeftCtrl) && IsKeyReleased(ImGuiKey_DownArrow) && history.size() > 0) {
        history.emplace_front() = std::move(history.back());
        history.pop_back();
        buffer = {history.back().begin(), history.back().end()};
        buffer.resize(buffer.size()+10,0);
        SetKeyboardFocusHere();
    }

    buffer.resize(1024*1024,0);

    if (!chat.running && IsWindowAppearing()) {
        SetKeyboardFocusHere();
    }

    if (InputTextMultiline("##input", buffer.data(), buffer.size(), inputSize, inputFlags) && !chat.running) {
        string input(trimWhite(buffer.data()));
        if (input == "/clear") {
            session.clear();
            ui.entries.clear();
        } else if (input == "/compact") {
            session.compact();
            ui.entries.push_back(
                {EntryType::Content, "[\u2302 compaction]", false, ui.next_seq++});
        } else if (input.size()) {
            ui.entries.push_back({EntryType::UserText, input, false, ui.next_seq++});
            start_chat(chat, session, input);
            for (auto it = history.begin(); it != history.end();
                it = *it == input ? history.erase(it): ++it);
            history.push_back(input);
            buffer.front() = 0;
        }
    }

    PopFont();
}
