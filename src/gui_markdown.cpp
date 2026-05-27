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
#include <md4c.h>
#include <string>

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
    ImFont* mono_font = nullptr;
    // code block rendering
    bool in_code_block = false;
    string code_buf;
    int code_blocks = 0;
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

        float width = GetContentRegionAvail().x - GetStyle().ItemSpacing.x;

        GetWindowDrawList()->AddRectFilled(
            GetCursorScreenPos(),
            GetCursorScreenPos() + ImVec2(width, GetFrameHeight()),
            GetColorU32(ImGuiCol_TableHeaderBg)
        );

        auto header_color = [&]() {
            if (h->level == 1) return IM_COL32(255, 255, 255, 255);
            if (h->level == 2) return IM_COL32(100, 100, 255, 255);
            if (h->level == 3) return IM_COL32(100, 200, 100, 255);
            if (h->level == 4) return IM_COL32(255, 100, 100, 255);
            return IM_COL32(100, 100, 100, 255);
        };

        float bar = GetStyle().ItemSpacing.x/4;

        for (int i = 0; i < h->level; i++) {
            GetWindowDrawList()->AddRectFilled(
                GetCursorScreenPos() + ImVec2(bar*i + bar*i, 0),
                GetCursorScreenPos() + ImVec2(bar*i + bar*i + bar, GetFrameHeight()),
                header_color()
            );
        }

        SetCursorPos(ImVec2(
            GetCursorPosX() + bar*h->level*2 - bar - bar + GetStyle().ItemSpacing.x,
            GetCursorPosY() + (GetFrameHeight()-GetTextLineHeight())/2)
        );

        PushTextWrapPos(0);
        ctx.style_depth++;
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        break;
    }
    case MD_BLOCK_CODE: {
        ctx.in_code_block = true;
        ctx.code_buf.clear();
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
        ImVec2 size(GetContentRegionAvail().x - GetStyle().ItemSpacing.x, 0);
        BeginTable(tid.c_str(), table->col_count, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, size);
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
        PushFont(ctx.mono_font);

        auto extents = CalcTextSize(ctx.code_buf.c_str());
        float height = std::min(GetTextLineHeightWithSpacing() * 15, extents.y);
        height += GetStyle().WindowPadding.y * 2;
        float width = GetContentRegionAvail().x-GetStyle().ItemSpacing.x;
        if (extents.x > width) height += GetStyle().ScrollbarSize;

        string id = std::to_string(++ctx.code_blocks);

        PushStyleColor(ImGuiCol_ChildBg, GetColorU32(ImGuiCol_TableRowBgAlt));
        BeginChild(id.c_str(),
            ImVec2(width, height),
            ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        PopStyleColor();

        TextUnformatted(ctx.code_buf.c_str());

        EndChild();
        PopFont();

        ctx.newline(type);
        ctx.in_code_block = false;
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
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        ctx.style_depth++;
        break;
    case MD_SPAN_CODE:
        PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 255, 255));
        PushFont(ctx.mono_font);
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
    case MD_SPAN_CODE:
        PopFont();
        PopStyleColor();
        ctx.style_depth--;
        break;
    case MD_SPAN_STRONG:
        PopStyleColor();
        ctx.style_depth--;
        break;
    case MD_SPAN_A:
    case MD_SPAN_DEL:
        PopStyleColor();
        ctx.style_depth--;
        break;
    default:
        break;
    }
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

static void render_plain_text_response(size_t seq, const std::string& text) {
    string id = "##text-block-" + std::to_string(seq);
    auto extents = CalcTextSize(text.c_str());
    float height = std::min(GetTextLineHeightWithSpacing() * 3, extents.y);
    height += GetStyle().WindowPadding.y * 2;
    float width = GetContentRegionAvail().x-GetStyle().ItemSpacing.x;
    if (extents.x > width) height += GetStyle().ScrollbarSize;
    PushStyleColor(ImGuiCol_ChildBg, GetColorU32(ImGuiCol_TableRowBgAlt));
    BeginChild(id.c_str(),
        ImVec2(width, height),
        ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    PopStyleColor();
    PushFont(mono_font);
    PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
    TextUnformatted(text.c_str());
    PopStyleColor();
    PopFont();
    EndChild();
}

// ── Render tool result inline (compact child window) ──
static void render_tool_result(size_t seq, const std::string& result, RenderToolResult mode) {
    int line_count = 1;
    for (char c : result) {
        if (c == '\n') line_count++;
    }

    float line_height = GetTextLineHeightWithSpacing();
    string id = "##toolresult-" + std::to_string(seq);

    switch (mode) {
        case RenderToolResult::None: break;

        case RenderToolResult::Plain: {
            render_plain_text_response(seq, result);
            break;
        }

        case RenderToolResult::Diff: {
            float height = GetTextLineHeightWithSpacing() * std::min(line_count, 15);
            height += GetStyle().WindowPadding.y * 2;

            PushStyleColor(ImGuiCol_ChildBg, GetColorU32(ImGuiCol_TableRowBgAlt));
            BeginChild(id.c_str(),
                ImVec2(GetContentRegionAvail().x-GetStyle().ItemSpacing.x, height),
                ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
            PopStyleColor();
            PushFont(mono_font);

            string_view cur(result);
            while (cur.size()) {
                auto sol = cur;
                while (cur.size() && cur.front() != '\n') {
                    cur.remove_prefix(1);
                }
                auto color = IM_COL32(160, 160, 160, 255);
                if (sol.starts_with("+")) {
                    color = IM_COL32(80, 200, 80, 255);
                }
                if (sol.starts_with("-")) {
                    color = IM_COL32(200, 60, 60, 255);
                }
                PushStyleColor(ImGuiCol_Text, color);
                TextUnformatted(sol.data(), cur.data());
                PopStyleColor();
                if (cur.size() && cur.front() == '\n') {
                    cur.remove_prefix(1);
                }
            }
            PopFont();
            EndChild();
            break;
        }
    }
}

static void render_tool_call_group(const ChatUIState& ui, size_t& i) {
    PushStyleColor(ImGuiCol_Text, IM_COL32(255, 165, 0, 255));
    PushTextWrapPos(0);
    PushFont(mono_font);
    size_t group_start = i;

    auto render_pair = [&]() {
        text_unformatted_ellipsis(ui.entries[i].text);
        if (ui.entries[i].tool_result.size()) {
            render_tool_result(i, ui.entries[i].tool_result, [&]() {
                if (string_view(ui.entries[i].text).contains("edit_file(")) {
                    return RenderToolResult::Diff;
                }
                if (string_view(ui.entries[i].text).contains("read_file(")) {
                    return RenderToolResult::None;
                }
                return RenderToolResult::Plain;
            }());
        }
    };

    render_pair();

    auto next_is_toolcall = [&]() {
        return i + 1 < ui.entries.size() && ui.entries[i + 1].type == EntryType::ToolCall;
    };

    while (next_is_toolcall()) {
        i++;
        NewLine();
        render_pair();
    }

    PopFont();
    NewLine();
    PopTextWrapPos();
    PopStyleColor();
}

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

    ensure_table_blank_lines(clean);

    RenderCtx ctx;
    ctx.mono_font = mono_font;

    MD_PARSER parser = {};
    parser.flags = MD_DIALECT_GITHUB | MD_FLAG_COLLAPSEWHITESPACE;
    parser.enter_block = enter_block_cb;
    parser.leave_block = leave_block_cb;
    parser.enter_span = enter_span_cb;
    parser.leave_span = leave_span_cb;
    parser.text = text_cb;

    PushStyleColor(ImGuiCol_Text, IM_COL32(200,200,200,255));
    md_parse(clean.data(), (MD_SIZE)clean.size(), &parser, &ctx);
    PopStyleColor();

    while (ctx.style_depth > 0) {
        PopStyleColor();
        ctx.style_depth--;
    }
}

void render_display_entry(const ChatUIState& ui, const DisplayEntry& entry, size_t& i, const string& you) {
    switch (entry.type) {
        case EntryType::UserText: {
            PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
            stringstream ss;
            ss << you << ": ";
            TextUnformatted(ss.str().c_str());
            PopStyleColor();
            SameLine(0,0);
            render_content(entry.text);
            break;
        }
        case EntryType::Reasoning: {
            PushStyleColor(ImGuiCol_Text, IM_COL32(50, 160, 50, 255));
            TextUnformatted("Thinking: ");
            if (entry.text.size()) SameLine(0,0);
            PopStyleColor();
            PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
            render_content(entry.text);
            PopStyleColor();
            if (!entry.text.size()) NewLine();
            break;
        }
        case EntryType::Content: {
            PushStyleColor(ImGuiCol_Text, IM_COL32(200, 80, 80, 255));
            TextUnformatted("Agent: ");
            if (entry.text.size()) SameLine(0,0);
            PopStyleColor();
            render_content(entry.text);
            break;
        }
        case EntryType::ToolCall: {
            render_tool_call_group(ui, i);
            break;
        }
    }
}
