#include "gui_chat.h"
#include ""
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
        if (ctx.mono_font)
            PushFont(ctx.mono_font);
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
        if (ctx.mono_font)
            PopFont();
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
        if (ctx.mono_font)
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
        if (ctx.mono_font)
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

    md_parse(clean.data(), (MD_SIZE)clean.size(), &parser, &ctx);

    while (ctx.style_depth > 0) {
        PopStyleColor();
        ctx.style_depth--;
    }
}
