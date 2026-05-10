#include "gui_chat.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <md4c.h>
#include <string>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

extern std::atomic<bool> g_interrupted;

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

namespace {

struct RenderCtx {
    int style_depth = 0;
    int tables = 0;
    // code block rendering
    bool in_code_block = false;
    string code_buf;
    ImVec2 code_start;
    float code_width;
    ImDrawListSplitter code_splitter;
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
        (void)h->level;
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
        Indent();
        NewLine();
        break;
    }
    case MD_BLOCK_HR:
        Separator();
        break;
    case MD_BLOCK_UL:
        break;
    case MD_BLOCK_OL:
        break;
    case MD_BLOCK_LI: {
        auto* li = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (li->is_task) {
            string mark = (li->task_mark == 'x' || li->task_mark == 'X') ? "[x] " : "[ ] ";
            TextUnformatted(mark.c_str());
        } else {
            Bullet();
        }
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
        Indent();
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
        NewLine();
        break;
    case MD_BLOCK_H:
        PopStyleColor();
        ctx.style_depth--;
        PopTextWrapPos();
        NewLine();
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
        NewLine();
        Unindent();
        ImVec2 br(GetCursorScreenPos().x, GetCursorScreenPos().y);
        auto* dl = GetWindowDrawList();
        ctx.code_splitter.SetCurrentChannel(dl, 0);
        dl->AddRectFilled(ctx.code_start, br, GetColorU32(ImGuiCol_FrameBgActive));
        ctx.code_splitter.Merge(dl);
        ctx.in_code_block = false;
        break;
    }
    case MD_BLOCK_HR:
        NewLine();
        break;
    case MD_BLOCK_UL:
        break;
    case MD_BLOCK_OL:
        break;
    case MD_BLOCK_LI:
        NewLine();
        break;
    case MD_BLOCK_TABLE:
        EndTable();
        NewLine();
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
        Unindent();
        NewLine();
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
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
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
        TextUnformatted(text, text + size);
        break;
    case MD_TEXT_CODE:
        if (ctx.in_code_block) {
            ctx.code_buf.append(text, size);
        } else {
            TextUnformatted(text, text + size);
        }
        break;
    case MD_TEXT_BR:
        NewLine();
        break;
    case MD_TEXT_SOFTBR:
        TextUnformatted(" ");
        break;
    case MD_TEXT_ENTITY:
        TextUnformatted(text, text + size);
        break;
    case MD_TEXT_NULLCHAR:
        TextUnformatted("\xef\xbf\xbd");
        break;
    case MD_TEXT_HTML:
    case MD_TEXT_LATEXMATH:
        break;
    }
    return 0;
}

} // anonymous namespace

void render_content(const string& text) {
    string clean = text;
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

static void drain_pending(ChatUIState& ui, AsyncChatState& chat) {
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

void render_chat_ui(ChatUIState& ui, AsyncChatState& chat, ChatSession& session, bool& done) {
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

    // ── mode toggle (Tab key, debounced to 500ms) ──
    {
        static std::chrono::steady_clock::time_point last_mode_toggle;
        if (!chat.running && IsKeyPressed(ImGuiKey_Tab, false)) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_mode_toggle > std::chrono::milliseconds(500)) {
                last_mode_toggle = now;
                Mode new_mode = (ui.mode == Mode::Plan) ? Mode::Build : Mode::Plan;
                ui.mode = new_mode;
                session.set_mode(new_mode);
            }
        }
    }

    // ── main window ──
    SetNextWindowPos(ImVec2(0, 0));
    SetNextWindowSize(GetIO().DisplaySize);
    Begin("llm-chat",
        nullptr,
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ── menu bar ──
    if (BeginMenuBar()) {
        if (BeginMenu("File")) {
            if (MenuItem("Exit", "Alt+F4"))
                done = true;
            EndMenu();
        }
        if (BeginMenu("Edit")) {
            if (MenuItem("Clear conversation")) {
                session.clear();
                ui.entries.clear();
            }
            if (MenuItem("Compact now")) {
                session.compact();
                ui.entries.push_back({EntryType::Content, "[\u2302 compactions]", false, ui.next_seq++});
            }
            EndMenu();
        }
        if (BeginMenu("Mode")) {
            Text("Current: %s", ui.mode == Mode::Plan ? "Plan" : "Build");
            Separator();
            if (MenuItem("Switch to Build (full access)", "Tab", false, ui.mode != Mode::Build)) {
                ui.mode = Mode::Build;
                session.set_mode(Mode::Build);
            }
            if (MenuItem("Switch to Plan (read-only)", "Tab", false, ui.mode != Mode::Plan)) {
                ui.mode = Mode::Plan;
                session.set_mode(Mode::Plan);
            }
            EndMenu();
        }
        if (BeginMenu("Model")) {
            bool changed = InputText(
                "Name", ui.model_buf, sizeof(ui.model_buf), ImGuiInputTextFlags_EnterReturnsTrue);
            SameLine();
            if (Button("Apply") || changed) {
                session.set_model(ui.model_buf);
            }
            Text("Current: %s", session.model().c_str());
            EndMenu();
        }
        EndMenuBar();
    }

    // ── tabs (Chat / Debug) ──
    float input_height = GetFrameHeightWithSpacing() * 3 + GetStyle().ItemSpacing.y * 2 +
        GetFrameHeightWithSpacing() + 8;

    if (ui.mono_font)
        PushFont(ui.mono_font);

    if (BeginTabBar("##tabs")) {
        // ── Chat tab ──
        if (BeginTabItem("Chat")) {
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

                if (i > 0)
                    NewLine();
                PushID(string("entry-" + std::to_string(i)).c_str());

                stringstream ss;

                switch (entry.type) {
                case EntryType::UserText:
                    PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
                    PushTextWrapPos(0);
                    ss << "You: " << entry.text;
                    TextUnformatted(ss.str().c_str());
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
                    PopTextWrapPos();
                    PopStyleColor();
                    break;
                }

                PopID();
            }

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
            EndTabItem();
        }

        // ── Raw tab ──
        if (BeginTabItem("Raw")) {
            BeginChild(
                "##raw", ImVec2(0, -input_height), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

            PushStyleColor(ImGuiCol_Text, IM_COL32(180, 180, 180, 255));

            size_t i = 0;

            if (ui.entries.size() > 30) {
                i = ui.entries.size() - 30;
                TextWrapped("%d old entries", int(i));
                Separator();
            }

            for (; i < ui.entries.size(); i++) {
                const auto& entry = ui.entries[i];
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

            EndChild();
            EndTabItem();
        }

        EndTabBar();
    }

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
            ImVec2(0, GetFrameHeightWithSpacing() * 3),
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

    if (chat.running) {
        if (Button("Cancel")) {
            cancel_chat(chat);
        }
    } else {
        bool disable_send = (ui.input_buf[0] == '\0');
        if (disable_send)
            BeginDisabled();
        if (Button("Send")) {
            string input(ui.input_buf);
            ui.input_buf[0] = '\0';
            if (!input.empty()) {
                ui.entries.push_back({EntryType::UserText, input, false, ui.next_seq++});
                start_chat(chat, session, std::move(input));
            }
        }
        if (disable_send)
            EndDisabled();
    }

    // ── mode indicator (same line as Cancel/Send) ──
    auto mode_str = (ui.mode == Mode::Plan) ? "[Plan]" : "[Build]";
    auto mode_color =
        (ui.mode == Mode::Plan) ? IM_COL32(100, 180, 255, 255) : IM_COL32(100, 255, 100, 255);
    SameLine(0, 16);
    TextColored(ImColor(mode_color), "%s", mode_str);

    // ── token usage indicator (after mode) ──
    {
        const auto& usage = session.last_usage();
        if (usage.total_tokens > 0) {
            SameLine(0, 8);
            TextColored(ImColor(IM_COL32(180, 180, 180, 255)), "[%d tokens]", usage.total_tokens);
        }
    }

    End(); // main window
}
