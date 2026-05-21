#include "gui_chat.h"
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

// ── InputText callback: track cursor position for insert-at-cursor ──────
static int InputTextCallback(ImGuiInputTextCallbackData* data) {
    auto* pos = static_cast<int*>(data->UserData);
    *pos = data->CursorPos;
    return 0;
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



void render_config_tab(PrimaryAgent& tab) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

    // ── Provider combo ──
    PushFont(mono_font);
    {
        string label = tab.provider_name.empty() ? cfg.providers[0].name : tab.provider_name;
        if (BeginCombo("Provider", label.c_str())) {
            for (const auto& p : cfg.providers) {
                bool is_selected = (p.name == tab.provider_name);
                if (Selectable(p.name.c_str(), is_selected)) {
                    if (p.name != tab.provider_name) {
                        // Provider changed — update session client
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
    PopFont();

    // ── Model combo (or manual text input if fetch failed) ──
    PushFont(mono_font);

    auto& cache_entry = g_provider_models[tab.provider_name];

    if (!cache_entry.fetched) {
        // Still loading
        PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        Text("Model:");
        SameLine();
        TextUnformatted("Loading models...");
        PopStyleColor();
    } else if (!cache_entry.error.empty() || cache_entry.models.empty()) {
        // Fetch failed or returned empty — show manual text input
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
        // Show dropdown
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
    PopFont();

    tab.validate_current_model();

    Separator();

    // ── Reasoning effort combo ──
    PushFont(mono_font);
    {
        std::string re =
            tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

        // Find which provider this tab uses so we can get its reasoning_efforts list
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
    PopFont();

    Separator();

    // Workspace path editing removed — safe_dir locked to cwd at startup

    // ── Compact / Clear / Stop buttons ──
    {
        Separator();

        // ── Compact button ──
        {
            int ctx_pct = session.context_usage_percent();
            string btn_label = "Compact (" + std::to_string(ctx_pct) + "% context used)";
            if (Button(btn_label.c_str())) {
                ui.compact_requested = true;
            }
        }

        // ── Clear Messages button ──
        {
            if (session.conversation().message_count() > 0) {
                int msg_count = static_cast<int>(session.conversation().message_count());
                string btn_label = "Clear Messages (" + std::to_string(msg_count) + ")";
                if (Button(btn_label.c_str())) {
                    ui.clear_requested = true;
                }
            } else {
                TextDisabled("No messages to clear");
            }
        }

        // ── Clear Plan button ──
        {
            auto plan_content = ::plan.read_plan();
            bool has_plan = plan_content && *plan_content != "(empty plan)";
            if (has_plan) {
                if (Button("Clear Plan")) {
                    ::plan.write_plan("");
                }
            } else {
                TextDisabled("No plan to clear");
            }
        }

        // ── Stop button ──
        {
            BeginDisabled(!tab.chat_state->running);
            if (Button("Stop")) {
                *tab.chat_state->cancelled = true;
            }
            EndDisabled();
        }
    }

    // ── Tool Gates section ──
    {
        Separator();
        Text("Tool Gates:");

        // Collect registered tool names, filter out skipped ones, and sort
        // by category first (so each group appears as one contiguous block),
        // then by name within each category.
        auto names = session.tools_for_testing().tool_names();

        // Categorisation helpers.
        auto category_of = [](const std::string& name) -> const char* {
            if (name == "list_directory" || name == "read_file" || name == "read_file_lines" ||
                name == "stat_file" || name == "grep_files" || name == "project_tree" ||
                name == "write_file" || name == "edit_file" || name == "delete_file" ||
                name == "move_file" || name == "rename_file" || name == "create_directory" ||
                name == "delete_directory")
                return "File";
            if (name == "git_status" || name == "git_diff" || name == "git_log" ||
                name == "git_show" || name == "git_add" || name == "git_commit" ||
                name == "git_restore")
                return "Git";
            if (name == "web_search" || name == "web_fetch")
                return "Web";
            if (name == "cmake_configure" || name == "cmake_build" || name == "cmake_ctest")
                return "Cmake";
            if (name == "run_bash" || name == "call_subagent")
                return "Execution";
            return "Other";
        };

        // Category display order.
        auto cat_order = [](const char* cat) -> int {
            if (!strcmp(cat, "Execution")) return 0;
            if (!strcmp(cat, "Cmake"))     return 1;
            if (!strcmp(cat, "File"))      return 2;
            if (!strcmp(cat, "Git"))       return 3;
            if (!strcmp(cat, "Web"))       return 4;
            return 5; // Other
        };

        // Filter and sort: first by category order, then alphabetically.
        names.erase(std::remove_if(names.begin(), names.end(),
            [](const std::string& name) {
                // Skip MCP tools (gated by server lifecycle), plan tools (always on),
                // view_tool_output (infrastructure), and cmd_* (shown separately).
                return name.rfind("mcp_", 0) == 0 || name == "read_plan" ||
                       name == "write_plan" || name == "view_tool_output" ||
                       name.rfind("cmd_", 0) == 0;
            }),
            names.end());
        std::sort(names.begin(), names.end(),
            [&](const std::string& a, const std::string& b) {
                int ca = cat_order(category_of(a));
                int cb = cat_order(category_of(b));
                if (ca != cb) return ca < cb;
                return a < b;
            });

        const char* last_cat = nullptr;
        for (const auto& name : names) {
            const char* cat = category_of(name);
            if (cat != last_cat) {
                Text("── %s ──", cat);
                last_cat = cat;
            }

            bool enabled = session.tool_enabled(name);
            if (Checkbox(name.c_str(), &enabled)) {
                tab.tool_gates[name] = enabled;
                session.set_tool_enabled(name, enabled);
                // Keep legacy gates in sync for tools with special filtering.
                if (name == "run_bash") {
                    tab.bash_enabled = enabled;
                    session.set_bash_enabled(enabled);
                } else if (name == "cmake_configure" || name == "cmake_build" || name == "cmake_ctest") {
                    tab.cmake_enabled = enabled;
                    session.set_cmake_enabled(enabled);
                }
            }
            if (IsItemHovered()) {
                BeginTooltip();
                TextUnformatted(name.c_str());
                EndTooltip();
            }
        }
    }

    // ── MCP Servers section ──
    if (!cfg.mcp_servers.empty()) {
        Separator();
        Text("MCP Servers:");
        for (const auto& mcp : cfg.mcp_servers) {
            bool enabled = tab.mcp_enabled[mcp.name];
            bool changed = Checkbox(mcp.name.c_str(), &enabled);
            if (changed) {
                tab.mcp_enabled[mcp.name] = enabled;
                tab.mcp_error.erase(mcp.name);
                if (enabled) {
                    auto result = session.start_mcp_server(mcp);
                    if (!result) {
                        tab.mcp_error[mcp.name] = result.error();
                        tab.mcp_enabled[mcp.name] = false; // revert checkbox
                    }
                } else {
                    session.stop_mcp_server(mcp.name);
                }
            }

            // Transport type label
            SameLine();
            TextDisabled("(%s)", mcp.transport.c_str());

            // Status / error
            if (session.mcp_registry().is_running(mcp.name)) {
                SameLine();
                TextColored(ImVec4(0, 1, 0, 1), "(*) running");
            } else if (tab.mcp_error.count(mcp.name)) {
                SameLine();
                TextColored(ImVec4(1, 0, 0, 1), "(!) %s", tab.mcp_error[mcp.name].c_str());
            }

            // Tooltip with server details
            if (IsItemHovered()) {
                BeginTooltip();
                Text("command: %s", mcp.command.c_str());
                if (!mcp.args.empty()) {
                    std::string args_str;
                    for (const auto& a : mcp.args) {
                        if (!args_str.empty())
                            args_str += " ";
                        args_str += a;
                    }
                    Text("args: %s", args_str.c_str());
                }
                if (!mcp.url.empty())
                    Text("url: %s", mcp.url.c_str());
                EndTooltip();
            }
        }
    }

    // ── Custom cmd_tools checkboxes ──
    if (!cfg.cmd_tools.empty()) {
        Separator();
        Text("Custom Commands:");
        for (const auto& ct : cfg.cmd_tools) {
            std::string tool_name = "cmd_" + ct.name;
            bool enabled = tab.cmd_tools_enabled[ct.name];
            bool changed = Checkbox(ct.name.c_str(), &enabled);
            if (changed) {
                tab.cmd_tools_enabled[ct.name] = enabled;
                session.set_custom_tool_enabled(tool_name, enabled);
            }
            if (IsItemHovered()) {
                BeginTooltip();
                TextUnformatted(ct.description.c_str());
                Text("command: %s", ct.command.c_str());
                EndTooltip();
            }
        }
    }

    // ── Session Custom Commands CRUD ──
    {
        Separator();
        Text("Session Custom Commands:");

        // Validation helper
        auto validate_cmd_name = [](const std::string& name) -> std::string {
            if (name.empty())
                return "Name must not be empty";
            for (char c : name) {
                if (std::isspace(static_cast<unsigned char>(c)))
                    return "Name must not contain spaces";
            }
            return {};
        };

        // ── Inline editor (Add / Edit) ──
        if (tab.cmd_edit.active) {
            PushID("cmd-edit");
            InputText("Name", tab.cmd_edit.name_buf.data(),
                tab.cmd_edit.name_buf.size());
            InputText("Description", tab.cmd_edit.desc_buf.data(),
                tab.cmd_edit.desc_buf.size());
            InputText("Command", tab.cmd_edit.command_buf.data(),
                tab.cmd_edit.command_buf.size());
            if (!tab.cmd_edit.error.empty()) {
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                TextUnformatted(tab.cmd_edit.error.c_str());
                PopStyleColor();
            }
            if (Button("Save")) {
                std::string name(tab.cmd_edit.name_buf.data());
                std::string desc(tab.cmd_edit.desc_buf.data());
                std::string command(tab.cmd_edit.command_buf.data());
                std::string err = validate_cmd_name(name);
                if (err.empty() && command.empty())
                    err = "Command must not be empty";
                if (!err.empty()) {
                    tab.cmd_edit.error = std::move(err);
                } else {
                    // Remove old entry if renaming
                    if (!tab.cmd_edit.original_name.empty() &&
                        tab.cmd_edit.original_name != name) {
                        tab.session_data.custom_commands.erase(
                            tab.cmd_edit.original_name);
                        // Unregister old tool name
                        session.unregister_custom_command(
                            tab.cmd_edit.original_name);
                    }
                    // Store new definition
                    CmdToolConfig cmd;
                    cmd.name = name;
                    cmd.description = desc;
                    cmd.command = command;
                    tab.session_data.custom_commands[name] = std::move(cmd);
                    // Register/update the tool live
                    session.register_custom_command(
                        name, desc, command, cfg.bash_timeout);
                    // Mark as enabled
                    tab.cmd_tools_enabled[name] = true;
                    tab.cmd_edit.active = false;
                    tab.cmd_edit.error.clear();
                }
            }
            SameLine();
            if (Button("Cancel")) {
                tab.cmd_edit.active = false;
                tab.cmd_edit.error.clear();
            }
            PopID();
        } else {
            if (Button("+ Add Command")) {
                tab.cmd_edit = {};
                tab.cmd_edit.active = true;
                tab.cmd_edit.original_name.clear();
                std::fill(tab.cmd_edit.name_buf.begin(), tab.cmd_edit.name_buf.end(), 0);
                std::fill(tab.cmd_edit.desc_buf.begin(), tab.cmd_edit.desc_buf.end(), 0);
                std::fill(tab.cmd_edit.command_buf.begin(), tab.cmd_edit.command_buf.end(), 0);
                tab.cmd_edit.error.clear();
            }
        }

        // ── List existing commands ──
        for (auto it = tab.session_data.custom_commands.begin();
             it != tab.session_data.custom_commands.end();) {
            PushID(it->first.c_str());
            // Enable/disable checkbox
            bool enabled = tab.cmd_tools_enabled[it->first];
            if (Checkbox("##en", &enabled)) {
                tab.cmd_tools_enabled[it->first] = enabled;
                session.set_custom_tool_enabled("cmd_" + it->first, enabled);
            }
            SameLine();
            // Name + description preview
            std::string preview = it->second.description;
            if (preview.size() > 60) {
                preview.resize(60);
                preview += "…";
            }
            Text("%s: %s", it->first.c_str(), preview.c_str());
            // Show overrides indicator
            bool masks_config = false;
            for (const auto& ct : cfg.cmd_tools) {
                if (ct.name == it->first) { masks_config = true; break; }
            }
            if (masks_config) {
                SameLine();
                TextDisabled("(overrides config)");
            }
            SameLine();
            if (Button("Edit")) {
                tab.cmd_edit.active = true;
                tab.cmd_edit.original_name = it->first;
                std::fill(tab.cmd_edit.name_buf.begin(), tab.cmd_edit.name_buf.end(), 0);
                std::fill(tab.cmd_edit.desc_buf.begin(), tab.cmd_edit.desc_buf.end(), 0);
                std::fill(tab.cmd_edit.command_buf.begin(), tab.cmd_edit.command_buf.end(), 0);
                std::copy(it->first.begin(), it->first.end(), tab.cmd_edit.name_buf.begin());
                std::copy(it->second.description.begin(), it->second.description.end(), tab.cmd_edit.desc_buf.begin());
                std::copy(it->second.command.begin(), it->second.command.end(), tab.cmd_edit.command_buf.begin());
                tab.cmd_edit.error.clear();
            }
            SameLine();
            if (Button("X")) {
                // Unregister tool (re-registers config version if exists)
                session.unregister_custom_command(it->first);
                tab.session_data.custom_commands.erase(it->first);
                tab.cmd_tools_enabled.erase(it->first);
                // Re-apply to gates
                session.set_custom_tool_enabled("cmd_" + it->first, false);
                // If config version exists, re-enable it by default
                for (const auto& ct : cfg.cmd_tools) {
                    if (ct.name == it->first) {
                        tab.cmd_tools_enabled[ct.name] = true;
                        session.set_custom_tool_enabled(
                            "cmd_" + ct.name, true);
                        break;
                    }
                }
                PopID();
                break; // iterator invalidated, restart loop
            }
            ++it;
            PopID();
        }
    }

    // ── Session Snippets CRUD ──
    {
        Separator();
        Text("Session Snippets:");

        // Validation helper
        auto validate_snippet_name = [](const std::string& name) -> std::string {
            if (name.empty())
                return "Name must not be empty";
            for (char c : name) {
                if (std::isspace(static_cast<unsigned char>(c)))
                    return "Name must not contain spaces";
                if (c == '!')
                    return "Name must not contain '!'";
            }
            return {};
        };

        // ── Inline editor (Add / Edit) ──
        if (tab.snippet_edit.active) {
            PushID("snippet-edit");
            InputText("Name", tab.snippet_edit.name_buf.data(),
                tab.snippet_edit.name_buf.size());
            InputText("Content", tab.snippet_edit.content_buf.data(),
                tab.snippet_edit.content_buf.size());
            if (!tab.snippet_edit.error.empty()) {
                PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                TextUnformatted(tab.snippet_edit.error.data());
                PopStyleColor();
            }
            if (Button("Save")) {
                std::string name(tab.snippet_edit.name_buf.data());
                std::string err = validate_snippet_name(name);
                if (!err.empty()) {
                    tab.snippet_edit.error = std::move(err);
                } else {
                    // Remove old entry if renaming an existing snippet
                    if (!tab.snippet_edit.original_name.empty())
                        tab.session_data.snippets.erase(tab.snippet_edit.original_name);
                    tab.session_data.snippets[name] =
                        std::string(tab.snippet_edit.content_buf.data());
                    tab.snippet_edit.active = false;
                    tab.snippet_edit.error.clear();
                }
            }
            SameLine();
            if (Button("Cancel")) {
                tab.snippet_edit.active = false;
                tab.snippet_edit.error.clear();
            }
            PopID();
        } else {
            if (Button("+ Add Snippet")) {
                tab.snippet_edit = {};
                tab.snippet_edit.active = true;
                tab.snippet_edit.original_name.clear();
                std::fill(tab.snippet_edit.name_buf.begin(), tab.snippet_edit.name_buf.end(), 0);
                std::fill(tab.snippet_edit.content_buf.begin(), tab.snippet_edit.content_buf.end(), 0);
                tab.snippet_edit.error.clear();
            }
        }

        // ── List existing snippets ──
        for (auto it = tab.session_data.snippets.begin();
             it != tab.session_data.snippets.end();) {
            PushID(it->first.c_str());
            if (Button("X")) {
                it = tab.session_data.snippets.erase(it);
                PopID();
                continue;
            }
            SameLine();
            // Show name and content preview (first 60 chars)
            std::string preview = it->second.substr(0, 60);
            if (it->second.size() > 60)
                preview += "…";
            Text("%s: \"%s\"", it->first.c_str(), preview.c_str());
            SameLine();
            if (Button("Edit")) {
                tab.snippet_edit.active = true;
                tab.snippet_edit.original_name = it->first;
                std::fill(tab.snippet_edit.name_buf.begin(), tab.snippet_edit.name_buf.end(), 0);
                std::fill(tab.snippet_edit.content_buf.begin(), tab.snippet_edit.content_buf.end(), 0);
                std::copy(it->first.begin(), it->first.end(), tab.snippet_edit.name_buf.begin());
                std::copy(it->second.begin(), it->second.end(), tab.snippet_edit.content_buf.begin());
                tab.snippet_edit.error.clear();
            }
            ++it;
            PopID();
        }
    }

    Separator();

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
    PushFont(mono_font);
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
    PopFont();

    // ── Model combo (or manual text input if fetch failed) ──
    PushFont(mono_font);

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
    PopFont();

    tab.validate_current_model();

    Separator();

    // ── Reasoning effort combo ──
    PushFont(mono_font);
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
    PopFont();
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

        switch (entry.type) {
        case EntryType::UserText:
            PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
            PushTextWrapPos(0);
            TextUnformatted(("You: " + entry.text).c_str());
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
            PushFont(mono_font);
            text_unformatted_ellipsis(entry.text);
            for (; i + 1 < ui.entries.size() && ui.entries[i + 1].type == EntryType::ToolCall;
                i++) {
                text_unformatted_ellipsis(ui.entries[i + 1].text);
            }
            PopFont();
            NewLine();
            PopTextWrapPos();
            PopStyleColor();
            break;
        }

        PopID();
    }

    NewLine();

    if (chat.running) {
        SameLine();
        TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Subagent is thinking...");
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
            PushFont(mono_font);
            text_unformatted_ellipsis(entry.text);
            for (; i + 1 < ui.entries.size() && ui.entries[i + 1].type == EntryType::ToolCall;
                i++) {
                text_unformatted_ellipsis(ui.entries[i + 1].text);
            }
            PopFont();
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

    float combo_width = (GetContentRegionAvail().x - GetStyle().ItemSpacing.x) / 2;

    // ── History combo (replaces input buf with history item) ──
    {
        auto& history = ui.input_history;
        SetNextItemWidth(combo_width);
        if (BeginCombo("##history-list", "history")) {
            for (const auto& entry : history) {
                if (Selectable(entry.c_str())) {
                    ui.input_buffer.assign(entry.begin(), entry.end());
                    ui.cursor_pos = entry.size();
                }
            }
            EndCombo();
        }
    }

    // ── Snippet reference combo (inserts !snippetname tag at cursor) ──
    // Merges config snippets and session snippets; session names override config.
    {
        // Build combined map: session snippets override config snippets
        auto all_snippets = cfg.snippets;
        for (const auto& [name, content] : tab.session_data.snippets) {
            all_snippets[name] = content; // session wins
        }
        if (!all_snippets.empty()) {
            SameLine(0, GetStyle().ItemSpacing.y);
            SetNextItemWidth(GetContentRegionAvail().x); // ~combo_width
            if (BeginCombo("##snippet-ref", "snippets")) {
                for (const auto& [name, content] : all_snippets) {
                    if (Selectable(name.c_str())) {
                        std::string tag = "!" + name;
                        insert_text_at_cursor(tag);
                    }
                }
                EndCombo();
            }
        }
    }

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

    ImVec2 inputSize(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4));

    if (InputTextMultiline("##input",
            buffer.data(),
            buffer.size(),
            inputSize,
            inputFlags,
            InputTextCallback,
            &ui.cursor_pos) &&
        !chat.running) {
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
        ImVec2 pos = GetCursorScreenPos() - ImVec2(0, GetFrameHeight()) +
            ImVec2(GetContentRegionAvail().x - GetStyle().FramePadding.x, 0);

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
}

// ── (Group Channel UI removed — agents communicate via send_message / next_message) ──
