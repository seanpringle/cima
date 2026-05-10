#include "gui_chat.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
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

void render_content(const string& text) {
    string copy = text;
    copy.erase(
        std::remove_if(copy.begin(), copy.end(), [](auto c) { return c == '\r' || c == '\0'; }),
        copy.end());

    string_view src(copy);

    auto canvas = GetContentRegionAvail();

    auto parseText = [&](string_view txt) {
        bool bold = false;
        bool code = false;
        int colors = 0;

        PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
        colors++;

        stringstream fragment;

        auto flush = [&]() {
            string str = fragment.str();
            fragment.str("");

            string_view prev(str);

            while (prev.size()) {
                auto next = prev;
                while (next.size() && !isspace(next.front()))
                    next.remove_prefix(1);
                if (next.size() && isspace(next.front()))
                    next.remove_prefix(1);
                string part(string_view(prev.data(), next.data() - prev.data()));
                auto size = CalcTextSize(part.c_str());
                if (GetCursorPos().x + size.x >= canvas.x)
                    NewLine();
                auto at = GetCursorPos();
                TextUnformatted(part.c_str());
                SetCursorPos(ImVec2(at.x + size.x, at.y));
                prev = next;
            }
        };

        while (txt.size()) {
            if (txt.starts_with("**") && !bold) {
                flush();
                txt.remove_prefix(2);
                bold = true;
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
                colors++;
                continue;
            }
            if (txt.starts_with("**") && bold) {
                flush();
                txt.remove_prefix(2);
                bold = false;
                PopStyleColor();
                colors--;
                continue;
            }

            if (txt.starts_with("`") && !code) {
                flush();
                txt.remove_prefix(1);
                code = true;
                PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 255, 255));
                colors++;
                continue;
            }
            if (txt.starts_with("`") && code) {
                flush();
                txt.remove_prefix(1);
                code = false;
                PopStyleColor();
                colors--;
                continue;
            }

            fragment << txt.front();
            txt.remove_prefix(1);
        }

        flush();
        PopStyleColor(colors);
    };

    auto scanLine = [&]() {
        string_view cur = src;
        while (cur.size() && cur.front() != '\n')
            cur.remove_prefix(1);
        string_view line(src.data(), cur.data() - src.data());
        if (cur.size() && cur.front() == '\n')
            cur.remove_prefix(1);
        src = cur;
        return line;
    };

    auto endBlock = [&]() {
        while (src.starts_with("\n")) {
            src.remove_prefix(1);
        }
        NewLine();
    };

    auto parseCodeBlock = [&]() {
        scanLine(); // ```
        auto drawList = GetWindowDrawList();
        ImDrawListSplitter splitter;
        splitter.Split(drawList, 2);
        splitter.SetCurrentChannel(drawList, 1);
        auto tl = GetCursorScreenPos();
        auto size = GetContentRegionAvail();
        Indent();
        NewLine();
        while (src.size()) {
            auto line = scanLine();
            if (line.starts_with("```"))
                break;
            TextUnformatted(line.data(), line.data() + line.size());
        }
        NewLine();
        Unindent();
        ImVec2 br(GetCursorScreenPos().x + size.x, GetCursorScreenPos().y);
        splitter.SetCurrentChannel(drawList, 0);
        drawList->AddRectFilled(tl, br, GetColorU32(ImGuiCol_FrameBgActive));
        splitter.Merge(drawList);
        endBlock();
    };

    int tables = 0;

    while (src.size()) {
        if (src.front() == '\n') {
            src.remove_prefix(1);
            continue;
        }

        if (src.starts_with("#")) {
            parseText(scanLine());
            endBlock();
            continue;
        }

        if (src.starts_with("```")) {
            parseCodeBlock();
            continue;
        }

        if (src.starts_with("---")) {
            scanLine();
            Separator();
            endBlock();
            continue;
        }

        if (src.starts_with("* ")) {
            while (src.starts_with("* ")) {
                Bullet();
                src.remove_prefix(2);
                parseText(scanLine());
                while (src.starts_with("  ")) {
                    NewLine();
                    NewLine();
                    src.remove_prefix(2);
                    parseText(scanLine());
                }
                NewLine();
            }
            endBlock();
            continue;
        }

        if (src.starts_with("- ")) {
            while (src.starts_with("- ")) {
                Bullet();
                src.remove_prefix(2);
                parseText(scanLine());
                while (src.starts_with("  ")) {
                    NewLine();
                    NewLine();
                    src.remove_prefix(2);
                    parseText(scanLine());
                }
                NewLine();
            }
            endBlock();
            continue;
        }

        auto numberedItem = [&]() {
            auto cursor = src;
            while (cursor.size() && std::isdigit(cursor.front()))
                cursor.remove_prefix(1);
            return cursor.data() != src.data() && cursor.size() && cursor.front() == '.';
        };

        if (numberedItem()) {
            Indent(CalcTextSize("_").x);
            while (numberedItem()) {
                parseText(scanLine());
                while (src.starts_with("  ")) {
                    NewLine();
                    NewLine();
                    src.remove_prefix(2);
                    parseText(scanLine());
                }
                NewLine();
            }
            endBlock();
            Unindent(CalcTextSize("_").x);
            continue;
        }

        auto tableSeparator = [&]() { return src.starts_with("|---") || src.starts_with("| ---"); };

        auto scanTableCell = [&]() {
            auto left = src;
            while (src.size() && !src.starts_with("|"))
                src.remove_prefix(1);
            auto right = src;
            string_view cell(left.data(), left.size() - right.size());
            while (cell.starts_with(" "))
                cell.remove_prefix(1);
            while (cell.ends_with(" "))
                cell.remove_suffix(1);
            return cell;
        };

        if (src.starts_with("|")) {
            vector<string_view> headers;
            while (src.starts_with("|") && !src.starts_with("|\n")) {
                src.remove_prefix(1);
                auto header = scanTableCell();
                if (!header.size())
                    break;
                headers.push_back(header);
            }
            scanLine();

            if (!headers.size()) {
                headers.emplace_back();
            }

            string tableId = "##table-" + std::to_string(++tables);
            BeginTable(
                tableId.c_str(), headers.size(), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg);
            for (auto [i, header] : headers | std::ranges::views::enumerate) {
                string title = string(header) + "##col-" + std::to_string(i);
                TableSetupColumn(title.c_str(),
                    i == headers.size() - 1 ? ImGuiTableColumnFlags_WidthStretch : 0);
            }

            while (src.starts_with("|")) {
                if (src.starts_with("|---") || src.starts_with("| ---")) {
                    scanLine();
                    TableHeadersRow();
                    continue;
                }

                TableNextRow();
                for (size_t i = 0; src.starts_with("|") && !src.starts_with("|\n"); i++) {
                    src.remove_prefix(1);
                    if (i >= headers.size()) {
                        scanTableCell();
                        break;
                    }
                    TableSetColumnIndex(i);
                    parseText(scanTableCell());
                }
                scanLine();
            }

            EndTable();
            NewLine();
            continue;
        }

        parseText(scanLine());
        endBlock();
        if (src.size())
            NewLine();
    }
}

static void render_code_block(const string& text) {
    if (text.empty())
        return;

    int lines = 1;
    for (char c : text)
        if (c == '\n')
            lines++;

    float line_height = GetTextLineHeightWithSpacing();
    float height = lines * line_height;

    ImVec2 start_pos = GetCursorScreenPos();
    float width = GetContentRegionAvail().x;

    ImDrawList* dl = GetWindowDrawList();
    dl->AddRectFilled(ImVec2(start_pos.x, start_pos.y),
        ImVec2(start_pos.x + width, start_pos.y + height + 8),
        IM_COL32(30, 30, 40, 255));

    PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
    SetCursorScreenPos(ImVec2(start_pos.x + 8, start_pos.y + 4));

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        string line = (nl == string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
        TextUnformatted(line.c_str());
        if (nl == string::npos)
            break;
        pos = nl + 1;
        SetCursorScreenPos(ImVec2(start_pos.x + 8, GetCursorScreenPos().y));
    }

    PopStyleColor();

    SetCursorScreenPos(ImVec2(start_pos.x, start_pos.y + height + 8));
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
