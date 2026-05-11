#include "gui_chat.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <md4c.h>
#include <string>
#include <future>
#include <thread>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

extern std::atomic<bool> g_interrupted;

namespace {

bool debug_markdown = false;

string dump(const string_view s) {
    std::span<const uint8_t> buf((const uint8_t*)s.data(), s.size());
    stringstream ss;
    for (size_t row = 0, lim = s.size() / 16 + std::min(size_t(1u), s.size() % 16); row < lim;
        row++) {
        for (size_t col = 0; col < 16; col++) {
            if (col == 8)
                ss << ' ';
            size_t i = row * 16 + col;
            if (i < buf.size()) {
                uint32_t b = buf[i];
                ss << std::hex << std::fixed << std::setw(2) << b << ' ';
            } else {
                ss << "   ";
            }
        }
        ss << ' ';
        for (size_t col = 0; col < 16; col++) {
            if (col == 8)
                ss << ' ';
            size_t i = row * 16 + col;
            if (i < buf.size()) {
                char b = buf[i];
                if (b > ' ' && b <= 'z') {
                    ss << b;
                } else {
                    ss << '.';
                }
            } else {
                ss << ' ';
            }
        }
        ss << '\n';
    }
    return ss.str();
};

void text_unformatted_ellipsis(const string& text) {
    auto canvas = GetContentRegionAvail();
    auto size = CalcTextSize(text.c_str());
    if (size.x < canvas.x) {
        TextUnformatted(text.c_str());
        return;
    }
    auto glyph = CalcTextSize("_");
    int cols = std::max(0, std::min(int(text.size()), int(canvas.x/glyph.x)-4));
    stringstream ss;
    ss << string_view(text.data(), cols) << "...";
    TextUnformatted(ss.str().c_str());
}

void text_unformatted_inline_wrap(const string& text) {
    auto blit = [&](string_view chunk) {
        auto size = CalcTextSize(chunk.data(), chunk.data() + chunk.size());
        if (!(GetContentRegionAvail().x > size.x)) NewLine();
        auto pos = GetCursorPos();
        TextUnformatted(chunk.data(), chunk.data() + chunk.size());
        SetCursorPos(ImVec2(pos.x + size.x, pos.y));
    };

    string_view cur(text);

    while (cur.size()) {
        if (std::isspace(cur.front())) {
            blit(string_view(cur.data(),1));
            cur.remove_prefix(1);
            continue;
        }
        auto left = cur;
        while (cur.size() && !std::isspace(cur.front())) cur.remove_prefix(1);
        blit(string_view(left.data(), left.size()-cur.size()));
    }
}

struct RenderCtx {
    int style_depth = 0;
    int tables = 0;
    // code block rendering
    bool in_code_block = false;
    string code_buf;
    ImVec2 code_start;
    float code_width;
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

    void newline(MD_BLOCKTYPE type) {
        if (debug_markdown) {
            GetWindowDrawList()->AddCircleFilled(GetCursorScreenPos(), 5, IM_COL32(255, 0, 0, 255));
            ImVec2 tl(GetCursorScreenPos().x-5, GetCursorScreenPos().y-5);
            ImVec2 br(GetCursorScreenPos().x+5, GetCursorScreenPos().y+5);
            if (IsMouseHoveringRect(tl, br) && BeginTooltip()) {
                string name = [&]() {
                    switch (type) {
                        case MD_BLOCK_DOC: return "MD_BLOCK_DOC";
                        case MD_BLOCK_QUOTE: return "MD_BLOCK_QUOTE";
                        case MD_BLOCK_UL: return "MD_BLOCK_UL";
                        case MD_BLOCK_OL: return "MD_BLOCK_OL";
                        case MD_BLOCK_LI: return "MD_BLOCK_LI";
                        case MD_BLOCK_HR: return "MD_BLOCK_HR";
                        case MD_BLOCK_H: return "MD_BLOCK_H";
                        case MD_BLOCK_CODE: return "MD_BLOCK_CODE";
                        case MD_BLOCK_HTML: return "MD_BLOCK_HTML";
                        case MD_BLOCK_P: return "MD_BLOCK_P";
                        case MD_BLOCK_TABLE: return "MD_BLOCK_TABLE";
                        case MD_BLOCK_THEAD: return "MD_BLOCK_THEAD";
                        case MD_BLOCK_TBODY: return "MD_BLOCK_TBODY";
                        case MD_BLOCK_TR: return "MD_BLOCK_TR";
                        case MD_BLOCK_TH: return "MD_BLOCK_TH";
                        case MD_BLOCK_TD: return "MD_BLOCK_TD";
                    }
                    return "(unknown)";
                }();
                Text("%s", name.c_str());
                EndTooltip();
            }
        }
        NewLine();
    }

    void newline(MD_TEXTTYPE type) {
        if (debug_markdown) {
            GetWindowDrawList()->AddCircleFilled(GetCursorScreenPos(), 5, IM_COL32(255, 0, 0, 255));
            ImVec2 tl(GetCursorScreenPos().x-5, GetCursorScreenPos().y-5);
            ImVec2 br(GetCursorScreenPos().x+5, GetCursorScreenPos().y+5);
            if (IsMouseHoveringRect(tl, br) && BeginTooltip()) {
                string name = [&]() {
                    switch (type) {
                        case MD_TEXT_BR: return "MD_TEXT_BR";
                        case MD_TEXT_SOFTBR: return "MD_TEXT_SOFTBR";
                        case MD_TEXT_NORMAL: return "MD_TEXT_NORMAL";
                        case MD_TEXT_NULLCHAR: return "MD_TEXT_NULLCHAR";
                        case MD_TEXT_ENTITY: return "MD_TEXT_ENTITY";
                        case MD_TEXT_CODE: return "MD_TEXT_CODE";
                        case MD_TEXT_HTML: return "MD_TEXT_HTML";
                        case MD_TEXT_LATEXMATH: return "MD_TEXT_LATEXMATH";
                    }
                    return "(unknown)";
                }();
                Text("%s", name.c_str());
                EndTooltip();
            }
        }
        NewLine();
    }
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
        ctx.code_width = GetContentRegionAvail().x;
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
        ctx.indent(after.x-before.x);
        break;
    }
    case MD_BLOCK_TABLE: {
        auto* table = static_cast<MD_BLOCK_TABLE_DETAIL*>(detail);
        string tid = "##tbl" + std::to_string(++ctx.tables);
        BeginTable(tid.c_str(), table->col_count,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
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
                string line = (nl == string::npos)
                    ? ctx.code_buf.substr(pos)
                    : ctx.code_buf.substr(pos, nl - pos);
                TextUnformatted(line.c_str());
                if (nl == string::npos)
                    break;
                pos = nl + 1;
                SetCursorScreenPos(
                    ImVec2(ctx.code_start.x + GetStyle().IndentSpacing,
                        GetCursorScreenPos().y));
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
        NewLine();
        NewLine();
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
    while (trim.size() && std::isspace(trim.back())) trim.remove_suffix(1);

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
    chat.future = std::async(std::launch::async,
        [&session, input = std::move(input)]() { return session.run_once(input); });
}

static void cancel_chat(AsyncChatState& chat) {
    g_interrupted = true;
    if (chat.future.valid()) {
        chat.future.wait();
        try {
            chat.future.get();
        } catch (const std::exception& e) {
            std::cerr << "chat error during cancel: " << e.what() << std::endl;
        }
    }
    chat.running = false;
    g_interrupted = false;
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

    // ── main content (previously inside "Chat" tab) ──
    float input_height = GetFrameHeightWithSpacing() * 6 + 8;

    if (ui.mono_font)
        PushFont(ui.mono_font);

    // ── toolbar ──
    if (Button("Clear")) {
        session.clear();
        ui.entries.clear();
    }
    SameLine();
    if (Button("Compact")) {
        session.compact();
        ui.entries.push_back({EntryType::Content, "[\u2302 compaction]", false, ui.next_seq++});
    }
    // ── Fetch models on first render ──
    if (!ui.models_loaded) {
        ui.models_loaded = true;
        // Fire off an async fetch so the UI stays responsive
        std::thread([&ui, &session]() {
            auto result = session.client_for_models().fetch_models();
            if (result) {
                ui.available_models = std::move(*result);
            } else {
                ui.models_error = std::move(result.error());
            }
            ui.models_fetched = true;
        }).detach();
    }

    // ── Model combo selector ──
    {
        SameLine();
        SetNextItemWidth(200);

        // Build a preview string (current model or fallback)
        std::string preview = session.model();
        if (preview.empty()) preview = "(select model)";

        PushID("model_combo");
        if (BeginCombo("##model", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            // Show a loading indicator if models haven't arrived yet
            if (!ui.models_fetched) {
                TextDisabled("Loading models...");
            } else if (!ui.models_error.empty()) {
                TextColored(ImColor(IM_COL32(255,100,100,255)), "Error: %s", ui.models_error.c_str());
            } else if (ui.available_models.empty()) {
                TextDisabled("(no models returned)");
            } else {
                for (const auto& m : ui.available_models) {
                    bool is_selected = (m == session.model());
                    if (Selectable(m.c_str(), is_selected)) {
                        session.set_model(m);
                        // Sync the model_buf so it stays in sync
                        strncpy(ui.model_buf, m.c_str(), sizeof(ui.model_buf) - 1);
                        // Update the tab title to reflect the new model
                        tab.title = m;
                    }
                    if (is_selected) {
                        SetItemDefaultFocus();
                    }
                }
            }
            EndCombo();
        }
        PopID();
    }

    // ── Raw popup toggle ──
    SameLine();
    if (Button("Raw")) {
        ui.show_raw_popup = true;
        std::cout << "Raw" << std::endl;
    }

    // ── token usage indicator ──
    {
        const auto& usage = session.last_usage();
        if (usage.total_tokens > 0) {
            SameLine(0, 8);
            TextColored(ImColor(IM_COL32(180, 180, 180, 255)), "[%d tokens]", usage.total_tokens);
        }
    }

    Separator();

    BeginChild("##chat",
        ImVec2(0, -input_height),
        false,
        ImGuiWindowFlags_AlwaysVerticalScrollbar);

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
            for (;
                i + 1 < ui.entries.size() && ui.entries[i + 1].type == EntryType::ToolCall;
                i++) {
                text_unformatted_ellipsis(ui.entries[i + 1].text);
            }
            NewLine();
            PopTextWrapPos();
            PopStyleColor();
            break;
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

    if (ui.mono_font)
        PopFont();

    // ── input area ──
    Separator();

    bool running_snapshot = chat.running;

    SetNextItemWidth(GetContentRegionAvail().x);

    if (running_snapshot)
        BeginDisabled();

    if (ui.mono_font)
        PushFont(ui.mono_font);

    if (InputTextMultiline("##input",
            ui.input_buf,
            sizeof(ui.input_buf),
            ImVec2(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4)),
            ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_WordWrap)) {
        string input(ui.input_buf);
        ui.input_buf[0] = '\0';
        if (!input.empty()) {
            ui.entries.push_back({EntryType::UserText, input, false, ui.next_seq++});
            start_chat(chat, session, std::move(input));
        }
    }

    if (ui.mono_font)
        PopFont();
    if (running_snapshot)
        EndDisabled();
}

void render_chat_overlay(TabInfo& tab, bool& done) {
    auto& ui = tab.ui_state;

    // ── Raw popup ──
    if (ui.show_raw_popup) {
        string raw_title = "Raw##" + std::to_string(tab.id);
        SetNextWindowPos(ImVec2(200, 200), ImGuiCond_Once);
        SetNextWindowSize(ImVec2(600, 400), ImGuiCond_Once);
        //SetNextWindowFocus();
        if (Begin(raw_title.c_str(), &ui.show_raw_popup)) {
            PushFont(ui.mono_font);
            PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));

            size_t ri = 0;

            if (ui.entries.size() > 30) {
                ri = ui.entries.size() - 30;
                TextWrapped("%d old entries", int(ri));
                Separator();
            }

            for (; ri < ui.entries.size(); ri++) {
                const auto& entry = ui.entries[ri];
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
                stringstream ss;
                ss << prefix << entry.text;
                TextUnformatted(ss.str().c_str());
                PopTextWrapPos();
            }
            PopStyleColor();
            PopFont();
        }
        End();
    }
}