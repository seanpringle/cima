#include "gui_chat.h"
#include "client.h"
#include "tools.h"
#include <cassert>
#include <iostream>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <md4c.h>
#include <string>
#include <thread>
#include <unordered_map>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

// ── Chat UI log persistence (JSON Lines format) ──────────────────────────

void ChatUIState::load_chat_log(const std::string& path) {
    log_path = path;
    entries.clear();
    int max_seq = 0;
    std::ifstream file(path);
    if (!file.is_open()) {
        next_seq = 1;
        return; // first run — no log yet
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            DisplayEntry e;
            e.seq = j.value("seq", 0);
            std::string t = j.value("type", "Content");
            if (t == "UserText")      e.type = EntryType::UserText;
            else if (t == "Reasoning") e.type = EntryType::Reasoning;
            else if (t == "Content")   e.type = EntryType::Content;
            else if (t == "ToolCall")  e.type = EntryType::ToolCall;
            else if (t == "Continuation") e.type = EntryType::Continuation;
            else continue;
            e.text = j.value("text", "");
            e.is_streaming = j.value("streaming", false);
            entries.push_back(std::move(e));
            if (e.seq > max_seq) max_seq = e.seq;
        } catch (...) {
            // skip corrupt line
        }
    }
    next_seq = max_seq + 1;
}

void ChatUIState::append_chat_log_entry(const DisplayEntry& entry) {
    if (log_path.empty()) return;
    json j;
    switch (entry.type) {
        case EntryType::UserText:     j["type"] = "UserText"; break;
        case EntryType::Reasoning:    j["type"] = "Reasoning"; break;
        case EntryType::Content:      j["type"] = "Content"; break;
        case EntryType::ToolCall:     j["type"] = "ToolCall"; break;
        case EntryType::Continuation: j["type"] = "Continuation"; break;
    }
    j["seq"] = entry.seq;
    j["text"] = entry.text;
    if (entry.is_streaming) {
        j["streaming"] = true;
    }
    std::ofstream file(log_path, std::ios::app);
    if (file.is_open()) {
        file << j.dump() << "\n";
    }
}

// ── InputText callback: track cursor position for insert-at-cursor ──────
static int InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* pos = static_cast<int*>(data->UserData);
    *pos = data->CursorPos;
    return 0;
}

// ── Helper: finalise a streaming entry and log it ────────────────────────
static void finalize_streaming_entry(ChatUIState& ui) {
    if (!ui.entries.empty() && ui.entries.back().is_streaming) {
        ui.entries.back().is_streaming = false;
        ui.append_chat_log_entry(ui.entries.back());
    }
}

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
    DisplayEntry entry{type, text, streaming, ui.next_seq++};
    ui.entries.push_back(entry);
    // Log non-streaming entries immediately; streaming entries are logged
    // when finalised (see finalize_streaming_entry).
    if (!streaming) {
        ui.append_chat_log_entry(entry);
    }
}

void drain_pending(ChatUIState& ui, AsyncChatState& chat) {
    std::lock_guard<std::mutex> lock(chat.mutex);
    for (auto& [pending_text, type] : chat.pending) {
        if (type == OutputType::ToolInvocation) {
            finalize_streaming_entry(ui);
            push_entry(ui, EntryType::ToolCall, pending_text, false);
        } else if (type == OutputType::Continuation) {
            finalize_streaming_entry(ui);
            push_entry(ui, EntryType::Continuation, pending_text, false);
        } else {
            auto entry_type =
                (type == OutputType::Reasoning) ? EntryType::Reasoning : EntryType::Content;
            if (!ui.entries.empty() && ui.entries.back().is_streaming &&
                ui.entries.back().type == entry_type) {
                ui.entries.back().text += pending_text;
            } else {
                finalize_streaming_entry(ui);
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
                    tab.model_name = ui.available_models.front();
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
                        // Update the model name
                        tab.model_name = m;
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

    // ── Chat state ──
    string stateInfo;
    ImU32 stateColor;
    if (tab.chat_state->running) {
        stateInfo = "running";
        stateColor = IM_COL32(100, 255, 100, 255); // green
    } else {
        stateInfo = "idle";
        stateColor = IM_COL32(180, 180, 180, 255); // grey
    }

    // ── Context usage percentage (from metadata, for token colour) ──
    int context_pct = 0;
    {
        auto meta = session.session_db().execute(
            "SELECT value FROM metadata WHERE key = 'context_usage_percent'");
        if (meta) {
            try {
                auto arr = json::parse(*meta);
                if (arr.is_array() && !arr.empty() && arr[0].contains("value")) {
                    context_pct = std::stoi(arr[0]["value"].get<std::string>());
                }
            } catch (...) {}
        }
    }

    ImU32 tokenColor;
    if (context_pct >= 90)
        tokenColor = IM_COL32(255, 68, 68, 255);      // red — critical
    else if (context_pct >= 60)
        tokenColor = IM_COL32(255, 208, 0, 255);       // yellow — warning
    else
        tokenColor = IM_COL32(180, 180, 180, 255);     // grey — normal

    // ── Token count & branch info ──
    string tokenInfo = std::to_string(session.last_usage().total_tokens) + " tokens";

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

    auto stateSize = CalcTextSize(stateInfo.c_str());
    auto branchInfoSize = CalcTextSize(branchInfo.c_str());
    auto sepSize = CalcTextSize(sep.c_str());
    auto tokenInfoSize = CalcTextSize(tokenInfo.c_str());

    // Lay out right-aligned: [branch] :: [tokens] :: [state]
    auto rightEdge = GetContentRegionMax().x - GetStyle().WindowPadding.x;

    auto branchPos = ImVec2(rightEdge - branchInfoSize.x,
        GetFrameHeight() / 2 - branchInfoSize.y / 2);
    auto sep1Pos = ImVec2(branchPos.x - sepSize.x, GetFrameHeight() / 2 - sepSize.y / 2);
    auto tokenPos = ImVec2(sep1Pos.x - tokenInfoSize.x, GetFrameHeight() / 2 - tokenInfoSize.y / 2);
    auto sep2Pos = ImVec2(tokenPos.x - sepSize.x, GetFrameHeight() / 2 - sepSize.y / 2);
    auto statePos = ImVec2(sep2Pos.x - stateSize.x, GetFrameHeight() / 2 - stateSize.y / 2);

    GetForegroundDrawList()->AddText(
        GetWindowPos() + statePos, stateColor, stateInfo.c_str());
    GetForegroundDrawList()->AddText(
        GetWindowPos() + sep2Pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());
    GetForegroundDrawList()->AddText(
        GetWindowPos() + tokenPos, tokenColor, tokenInfo.c_str());
    GetForegroundDrawList()->AddText(
        GetWindowPos() + sep1Pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());
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
        finalize_streaming_entry(ui);
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
            case EntryType::Continuation:
                prefix = "[Continuing] ";
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
            case EntryType::Continuation:
                PushStyleColor(ImGuiCol_Text, IM_COL32(100, 255, 100, 255));
                PushTextWrapPos(0);
                ss << entry.text;
                TextUnformatted(ss.str().c_str());
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

    // ── Wiki page reference combo (inserts page name at cursor) ──
    {
        auto wiki = tab.session->wiki();
        if (wiki) {
            auto pages_result = wiki->list_pages();
            if (pages_result && !pages_result->empty()) {
                static string selected_page;
                string preview = selected_page.empty() ? "Wiki@..." : selected_page;
                SetNextItemWidth(GetContentRegionAvail().x);
                if (BeginCombo("##wiki-ref", preview.c_str())) {
                    for (const auto& page : *pages_result) {
                        bool is_selected = (page == selected_page);
                        if (Selectable(page.c_str(), is_selected)) {
                            selected_page = page;
                            // Insert page name at cursor position (no trailing space)
                            auto& buf = ui.input_buffer;
                            int pos = ui.cursor_pos;
                            if (pos < 0 || (size_t)pos > strlen(buf.data()))
                                pos = (int)strlen(buf.data()); // append
                            // Make room and insert into buffer
                            size_t room = buf.size() - strlen(buf.data()) - 1;
                            size_t insert_len = page.size();
                            if (insert_len > room) insert_len = room;
                            memmove(buf.data() + pos + insert_len,
                                    buf.data() + pos,
                                    strlen(buf.data()) - pos + 1);
                            memcpy(buf.data() + pos, page.data(), insert_len);
                            ui.cursor_pos = pos + (int)insert_len;
                        }
                    }
                    EndCombo();
                }
            }
        }
    }

    // ── Snippet reference combo (inserts content at cursor) ──
    {
        auto wiki = tab.session->wiki();
        if (wiki) {
            auto snippets_result = wiki->list_snippets();
            if (snippets_result && !snippets_result->empty()) {
                static string selected_snippet;
                string preview = selected_snippet.empty() ? "Snippet@..." : selected_snippet;
                SetNextItemWidth(GetContentRegionAvail().x);
                if (BeginCombo("##snippet-ref", preview.c_str())) {
                    for (const auto& [name, content] : *snippets_result) {
                        bool is_selected = (name == selected_snippet);
                        if (Selectable(name.c_str(), is_selected)) {
                            selected_snippet = name;
                            auto& buf = ui.input_buffer;
                            int pos = ui.cursor_pos;
                            if (pos < 0 || (size_t)pos > strlen(buf.data()))
                                pos = (int)strlen(buf.data());
                            size_t room = buf.size() - strlen(buf.data()) - 1;
                            size_t insert_len = content.size();
                            if (insert_len > room) insert_len = room;
                            memmove(buf.data() + pos + insert_len,
                                    buf.data() + pos,
                                    strlen(buf.data()) - pos + 1);
                            memcpy(buf.data() + pos, content.data(), insert_len);
                            ui.cursor_pos = pos + (int)insert_len;
                        }
                    }
                    EndCombo();
                }
            }
        }
    }

    SetNextItemWidth(GetContentRegionAvail().x);

    PushFont(ui.mono_font);

    uint32_t inputFlags = ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap |
        ImGuiInputTextFlags_CallbackAlways;

    ImVec2 inputSize(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4));

    auto trimWhite = [](string_view cur) -> string_view {
        while (cur.size() && isspace(cur.front())) cur.remove_prefix(1);
        while (cur.size() && isspace(cur.back())) cur.remove_suffix(1);
        return cur;
    };

    auto& buffer = ui.input_buffer;
    auto& history = ui.input_history;

    // Ensure adequate capacity once; avoid per-frame resize
    constexpr size_t kInputBufferSize = 1024 * 1024;
    if (buffer.size() < kInputBufferSize) {
        buffer.resize(kInputBufferSize, 0);
    }

    if (IsKeyDown(ImGuiKey_LeftCtrl) && IsKeyReleased(ImGuiKey_UpArrow) && history.size() > 0) {
        history.emplace_back() = std::move(history.front());
        history.pop_front();
        // Copy history entry into buffer, ensuring null termination + adequate size
        auto& hist_entry = history.back();
        buffer.assign(hist_entry.begin(), hist_entry.end());
        if (buffer.size() < kInputBufferSize) {
            buffer.resize(kInputBufferSize, 0);
        } else {
            buffer.push_back('\0');
        }
        SetKeyboardFocusHere();
    }

    if (IsKeyDown(ImGuiKey_LeftCtrl) && IsKeyReleased(ImGuiKey_DownArrow) && history.size() > 0) {
        history.emplace_front() = std::move(history.back());
        history.pop_back();
        auto& hist_entry = history.back();
        buffer.assign(hist_entry.begin(), hist_entry.end());
        if (buffer.size() < kInputBufferSize) {
            buffer.resize(kInputBufferSize, 0);
        } else {
            buffer.push_back('\0');
        }
        SetKeyboardFocusHere();
    }

    if (!chat.running && IsWindowAppearing()) {
        SetKeyboardFocusHere();
    }

    if (InputTextMultiline("##input", buffer.data(), buffer.size(), inputSize, inputFlags, InputTextCallback, &ui.cursor_pos) && !chat.running) {
        string input(trimWhite(buffer.data()));
        if (input.size()) {
            push_entry(ui, EntryType::UserText, input, false);
            start_chat(chat, session, input);
            for (auto it = history.begin(); it != history.end();
                it = *it == input ? history.erase(it): ++it);
            history.push_back(input);
            buffer.front() = 0;


        }
    }

    PopFont();
}

void render_session_db_view(SessionDB& db) {
    // ── Table list ──
    auto tables_result = db.execute("SELECT name, sql FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    if (!tables_result) {
        TextDisabled("(database unavailable)");
        return;
    }

    json tables_json;
    try {
        tables_json = json::parse(*tables_result);
    } catch (...) {
        TextDisabled("(error reading schema)");
        return;
    }

    if (!tables_json.is_array() || tables_json.empty()) {
        TextDisabled("(no tables)");
        return;
    }

    // Build table name list and remember selected index
    static string selected_table;
    vector<string> table_names;
    for (const auto& t : tables_json) {
        table_names.push_back(t.value("name", ""));
    }

    // Ensure selected_table is still valid
    bool found = false;
    for (const auto& n : table_names) {
        if (n == selected_table) {
            found = true;
            break;
        }
    }
    if (!found && !table_names.empty()) {
        selected_table = table_names.front();
    }

    // Combo to select a table
    if (table_names.empty())
        return;

    int current = 0;
    for (int i = 0; i < (int)table_names.size(); i++) {
        if (table_names[i] == selected_table) {
            current = i;
            break;
        }
    }
    string combo_label = "##db-table-select";
    PushItemWidth(GetContentRegionAvail().x);
    if (BeginCombo(combo_label.c_str(), selected_table.c_str())) {
        for (int i = 0; i < (int)table_names.size(); i++) {
            bool is_selected = (table_names[i] == selected_table);
            if (Selectable(table_names[i].c_str(), is_selected)) {
                selected_table = table_names[i];
            }
            if (is_selected) {
                SetItemDefaultFocus();
            }
        }
        EndCombo();
    }
    PopItemWidth();

    if (selected_table.empty())
        return;

    SeparatorText("Schema");

    // ── Schema (PRAGMA table_info) ──
    string pragma_sql = "PRAGMA table_info(" + selected_table + ")";
    auto schema_result = db.execute(pragma_sql);
    if (!schema_result) {
        TextDisabled("(schema unavailable)");
        return;
    }

    json schema_json;
    try {
        schema_json = json::parse(*schema_result);
    } catch (...) {
        TextDisabled("(error parsing schema)");
        return;
    }

    struct ColInfo {
        string name;
        string type;
        bool notnull;
        bool pk;
    };
    vector<ColInfo> cols;
    if (schema_json.is_array()) {
        for (const auto& row : schema_json) {
            ColInfo ci;
            ci.name = row.value("name", "");
            ci.type = row.value("type", "");
            ci.notnull = row.value("notnull", 0) != 0;
            ci.pk = row.value("pk", 0) != 0;
            cols.push_back(ci);
        }
    }

    if (cols.empty()) {
        TextDisabled("(no columns)");
        return;
    }

    // Render schema as a small table
    if (BeginTable("##db-schema", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        TableSetupColumn("Column");
        TableSetupColumn("Type");
        TableSetupColumn("Not Null");
        TableSetupColumn("PK");
        TableHeadersRow();

        for (const auto& ci : cols) {
            TableNextRow();
            TableNextColumn();
            TextUnformatted(ci.name.c_str());
            TableNextColumn();
            TextUnformatted(ci.type.c_str());
            TableNextColumn();
            TextUnformatted(ci.notnull ? "\xe2\x9c\x93" : "");
            TableNextColumn();
            TextUnformatted(ci.pk ? "\xe2\x9c\x93" : "");
        }
        EndTable();
    }

    SeparatorText("Data");

    // ── Data (SELECT * with LIMIT) ──
    string data_sql = "SELECT * FROM \"" + selected_table + "\" LIMIT 200";
    auto data_result = db.execute(data_sql);
    if (!data_result) {
        TextDisabled("(data unavailable)");
        return;
    }

    json data_json;
    try {
        data_json = json::parse(*data_result);
    } catch (...) {
        TextDisabled("(error parsing data)");
        return;
    }

    if (!data_json.is_array()) {
        TextDisabled("(no data)");
        return;
    }

    size_t row_count = data_json.size();
    TextUnformatted((std::to_string(row_count) + " rows").c_str());

    if (row_count == 0)
        return;

    // ── Compute intelligent column widths ──
    // For each column, find the max display-string length (in characters)
    // across the header name and all data values. Use these as stretch
    // weights so wider content gets more space proportionally.
    vector<float> col_weights;
    col_weights.reserve(cols.size());
    for (const auto& ci : cols) {
        size_t max_len = ci.name.size(); // header name
        for (const auto& row : data_json) {
            if (!row.contains(ci.name))
                continue;
            const auto& val = row[ci.name];
            size_t len = 0;
            if (val.is_null()) {
                len = 4; // "NULL"
            } else if (val.is_string()) {
                string s = val.get<string>();
                len = std::min<size_t>(s.size(), 500);
                if (s.size() > 500) len += 3; // "..."
            } else if (val.is_number()) {
                if (val.is_number_integer()) {
                    len = std::to_string((long long)val.get<int64_t>()).size();
                } else {
                    char buf[64];
                    len = (size_t)snprintf(buf, sizeof(buf), "%g", val.get<double>());
                }
            } else if (val.is_boolean()) {
                len = val.get<bool>() ? 4 : 5; // "true" / "false"
            } else {
                string s = val.dump();
                len = std::min<size_t>(s.size(), 500);
                if (s.size() > 500) len += 3;
            }
            if (len > max_len)
                max_len = len;
        }
        // Add a small headroom (2 chars) so columns aren't cramped
        col_weights.push_back(std::max((float)max_len + 2.0f, 1.0f));
    }

    auto canvas = GetContentRegionAvail();
    // Use the full remaining height so the table fills the panel vertically
    ImVec2 outer_size(canvas.x, std::max(canvas.y - 4.0f, 60.0f));
    if (BeginTable("##db-data", (int)cols.size(),
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_Resizable,
            outer_size)) {
        // Column headers with proportional stretch weights
        for (size_t i = 0; i < cols.size(); i++) {
            TableSetupColumn(cols[i].name.c_str(),
                ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,
                col_weights[i]);
        }
        TableSetupScrollFreeze(0, 1); // freeze header row
        TableHeadersRow();

        // Data rows
        for (const auto& row : data_json) {
            TableNextRow();
            for (size_t ci = 0; ci < cols.size(); ci++) {
                TableNextColumn();
                if (row.contains(cols[ci].name)) {
                    const auto& val = row[cols[ci].name];
                    if (val.is_null()) {
                        TextDisabled("NULL");
                    } else if (val.is_string()) {
                        string s = val.get<string>();
                        // Truncate very long strings
                        if (s.size() > 500) {
                            s.resize(500);
                            s += "...";
                        }
                        TextUnformatted(s.c_str());
                    } else if (val.is_number()) {
                        if (val.is_number_integer()) {
                            Text("%lld", (long long)val.get<int64_t>());
                        } else {
                            Text("%g", val.get<double>());
                        }
                    } else if (val.is_boolean()) {
                        TextUnformatted(val.get<bool>() ? "true" : "false");
                    } else {
                        string s = val.dump();
                        if (s.size() > 500) {
                            s.resize(500);
                            s += "...";
                        }
                        TextUnformatted(s.c_str());
                    }
                }
            }
        }
        EndTable();
    }
}

// ── (Group Channel UI removed — agents communicate via send_message / next_message) ──
