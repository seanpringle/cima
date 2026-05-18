#include "gui_chat.h"
#include "client.h"
#include "tools.h"
#include <cassert>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <map>
#include <md4c.h>
#include <string>
#include <thread>

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

// ── Chat UI log persistence (now handled via AssistantData) ──────────────

void ChatUIState::load_chat_log(const std::string& /*path*/) {
    // No-op: chat log persistence is now handled externally via AssistantData.
    // Entries are managed in-memory; they are loaded/saved as part of the
    // consolidated per-assistant JSON file.
}

void ChatUIState::append_chat_log_entry(const DisplayEntry& /*entry*/) {
    // No-op: chat log persistence is now handled externally via AssistantData.
    // Entries remain in the in-memory `entries` vector and are saved during
    // tab close / application shutdown.
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
        if (ctx.mono_font) PushFont(ctx.mono_font);
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
        if (ctx.mono_font) PopFont();
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
        if (ctx.mono_font) PushFont(ctx.mono_font);
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
        if (ctx.mono_font) PopFont();
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

void render_content(const string& text, ImFont* mono_font) {
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

void render_config_tab(PrimaryAgent& tab, const Config& cfg, ImFont* mono_font) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

    // ── Helper: start fetching models for the current provider ──
    auto trigger_model_fetch = [&]() {
        ui.models_loaded = true;
        ui.models_validated = false;
        ui.models_fetched->store(false, std::memory_order_release);
        ui.available_models.clear();
        ui.models_error.clear();

        // Build a ChatClient for the provider this tab is using
        // (we need to find the provider in cfg.providers by name)
        std::string api_base;
        std::string api_key;
        for (const auto& p : cfg.providers) {
            if (p.name == tab.provider_name) {
                api_base = p.api_base;
                api_key = p.api_key;
                break;
            }
        }
        if (api_base.empty()) {
            // Fallback to first provider
            api_base = cfg.providers[0].api_base;
            api_key = cfg.providers[0].api_key;
        }

        std::weak_ptr<bool> weak_tab = ui.tab_alive;
        std::packaged_task<void()> task([&ui, weak_tab, api_base, api_key]() {
            // Check if tab is still alive before accessing ui
            if (weak_tab.expired()) return;
            ChatClient client(api_base, api_key);
            auto result = client.fetch_models();
            if (!weak_tab.lock()) return; // tab was destroyed mid-fetch
            if (result) {
                ui.available_models = std::move(*result);
            } else {
                ui.models_error = std::move(result.error());
            }
            ui.models_fetched->store(true, std::memory_order_release);
        });
        ui.models_future = task.get_future();
        std::thread(std::move(task)).detach();
    };

    // ── Fetch models on first render ──
    if (!ui.models_loaded) {
        trigger_model_fetch();
    }

    // ── Provider combo ──
    PushFont(mono_font);
    {
        string label = tab.provider_name.empty() ? cfg.providers[0].name : tab.provider_name;
        if (BeginCombo("Provider", label.c_str())) {
            for (const auto& p : cfg.providers) {
                bool is_selected = (p.name == tab.provider_name);
                if (Selectable(p.name.c_str(), is_selected)) {
                    if (p.name != tab.provider_name) {
                        // Provider changed — update session client and re-fetch models
                        session.set_provider(p);
                        tab.provider_name = p.name;
                        tab.model_name = p.model;
                        tab.reasoning_effort = p.reasoning_effort;
                        trigger_model_fetch();
                    }
                }
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

    // ── Model combo (or manual text input if fetch failed) ──
    PushFont(mono_font);

    if (!ui.models_fetched->load(std::memory_order_acquire)) {
        // Still loading
        PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        Text("Model:");
        SameLine();
        TextUnformatted("Loading models...");
        PopStyleColor();
    } else if (!ui.models_error.empty() || ui.available_models.empty()) {
        // Fetch failed or returned empty — show manual text input
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        Text("Model:");
        SameLine();
        if (!ui.models_error.empty()) {
            TextUnformatted(ui.models_error.c_str());
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
            for (const auto& m : ui.available_models) {
                bool is_selected = (m == session.model());
                if (Selectable(m.c_str(), is_selected)) {
                    session.set_model(m);
                    tab.model_name = m;
                }
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

    // Validate current model selection (auto-select first if current not found)
    if (ui.models_fetched->load(std::memory_order_acquire) && !ui.models_validated &&
        !ui.available_models.empty()) {
        ui.models_validated = true;
        const auto& current = session.model();
        bool found = std::any_of(ui.available_models.begin(), ui.available_models.end(),
            [&current](const std::string& m) { return m == current; });
        if (!found) {
            session.set_model(ui.available_models.front());
            tab.model_name = ui.available_models.front();
        }
    }

    Separator();

    // ── Reasoning effort combo ──
    PushFont(mono_font);
    {
        std::string re = tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

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
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

    Separator();

    // Workspace path editing removed — safe_dir locked to cwd at startup

    Separator();

    // ── Bash checkbox ──
    {
        bool enabled = tab.bash_enabled;
        if (Checkbox("Enable run_bash (shell command execution)", &enabled)) {
            tab.bash_enabled = enabled;
            session.set_bash_enabled(enabled);
        }
        if (IsItemHovered()) {
            BeginTooltip();
            TextUnformatted("When disabled, the model cannot execute shell commands via run_bash.");
            EndTooltip();
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
                TextColored(ImVec4(0,1,0,1), "(*) running");
            } else if (tab.mcp_error.count(mcp.name)) {
                SameLine();
                TextColored(ImVec4(1,0,0,1), "(!) %s", tab.mcp_error[mcp.name].c_str());
            }

            // Tooltip with server details
            if (IsItemHovered()) {
                BeginTooltip();
                Text("command: %s", mcp.command.c_str());
                if (!mcp.args.empty()) {
                    std::string args_str;
                    for (const auto& a : mcp.args) {
                        if (!args_str.empty()) args_str += " ";
                        args_str += a;
                    }
                    Text("args: %s", args_str.c_str());
                }
                if (!mcp.url.empty()) Text("url: %s", mcp.url.c_str());
                EndTooltip();
            }
        }
    }

    Separator();

    Separator();

    // ── Compact button ──
    {
        if (ui.compacting) {
            TextDisabled("Compacting...");
        } else {
            int ctx_pct = session.context_usage_percent();
            string btn_label = "Compact (" + std::to_string(ctx_pct) + "% context used)";
            if (Button(btn_label.c_str())) {
                ui.compact_requested = true;
            }
        }
    }

    // ── Clear button ──
    {
        if (ui.clearing) {
            TextDisabled("Clearing...");
        } else if (session.conversation().message_count() > 0) {
            int msg_count = static_cast<int>(session.conversation().message_count());
            string btn_label = "Clear (" + std::to_string(msg_count) + " messages)";
            if (Button(btn_label.c_str())) {
                ui.clear_requested = true;
            }
        } else {
            TextDisabled("No messages to clear");
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

// ── Tag expansion for !snippet-name references ──
// Expands !snippetname to the full snippet content.  Non-matching tags are left as-is.
static std::string expand_tags(std::string input) {
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
                auto it = cfg.snippets.find(name);
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

void render_subagent_tab(SubAgent& tab, const Config& cfg, ImFont* mono_font) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

    // ── Helper: start fetching models for the current provider ──
    auto trigger_model_fetch = [&]() {
        ui.models_loaded = true;
        ui.models_validated = false;
        ui.models_fetched->store(false, std::memory_order_release);
        ui.available_models.clear();
        ui.models_error.clear();

        std::string api_base;
        std::string api_key;
        for (const auto& p : cfg.providers) {
            if (p.name == tab.provider_name) {
                api_base = p.api_base;
                api_key = p.api_key;
                break;
            }
        }
        if (api_base.empty()) {
            api_base = cfg.providers[0].api_base;
            api_key = cfg.providers[0].api_key;
        }

        std::weak_ptr<bool> weak_tab = ui.tab_alive;
        std::packaged_task<void()> task([&ui, weak_tab, api_base, api_key]() {
            // Check if tab is still alive before accessing ui
            if (weak_tab.expired()) return;
            ChatClient client(api_base, api_key);
            auto result = client.fetch_models();
            if (!weak_tab.lock()) return; // tab was destroyed mid-fetch
            if (result) {
                ui.available_models = std::move(*result);
            } else {
                ui.models_error = std::move(result.error());
            }
            ui.models_fetched->store(true, std::memory_order_release);
        });
        ui.models_future = task.get_future();
        std::thread(std::move(task)).detach();
    };

    // ── Fetch models on first render ──
    if (!ui.models_loaded) {
        trigger_model_fetch();
    }

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
                        trigger_model_fetch();
                    }
                }
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

    // ── Model combo (or manual text input if fetch failed) ──
    PushFont(mono_font);

    if (!ui.models_fetched->load(std::memory_order_acquire)) {
        PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        Text("Model:");
        SameLine();
        TextUnformatted("Loading models...");
        PopStyleColor();
    } else if (!ui.models_error.empty() || ui.available_models.empty()) {
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        Text("Model:");
        SameLine();
        if (!ui.models_error.empty()) {
            TextUnformatted(ui.models_error.c_str());
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
            for (const auto& m : ui.available_models) {
                bool is_selected = (m == session.model());
                if (Selectable(m.c_str(), is_selected)) {
                    session.set_model(m);
                    tab.model_name = m;
                }
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

    // Validate current model selection (auto-select first if current not found)
    if (ui.models_fetched->load(std::memory_order_acquire) && !ui.models_validated &&
        !ui.available_models.empty()) {
        ui.models_validated = true;
        const auto& current = session.model();
        bool found = std::any_of(ui.available_models.begin(), ui.available_models.end(),
            [&current](const std::string& m) { return m == current; });
        if (!found) {
            session.set_model(ui.available_models.front());
            tab.model_name = ui.available_models.front();
        }
    }

    Separator();

    // ── Reasoning effort combo ──
    PushFont(mono_font);
    {
        std::string re = tab.reasoning_effort.empty()
                             ? session.reasoning_effort()
                             : tab.reasoning_effort;

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
                if (is_selected) SetItemDefaultFocus();
            }
            EndCombo();
        }
    }
    PopFont();

}

void render_subagent_chat(SubAgent& tab, ImFont* mono_font) {
    auto& ui = tab.ui_state;
    auto& chat = *tab.chat_state;

    // Drain any pending streaming output first
    drain_pending(ui, chat);

    // Check if subagent chat finished
    if (chat.running && chat.future.valid() &&
        chat.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = chat.future.get();
            (void)result; // result already streamed via callbacks
        } catch (const std::exception& e) {
            push_entry(ui, EntryType::Content, "Error: " + std::string(e.what()), false);
        }
        chat.running = false;
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
            render_content("Thinking: " + entry.text, mono_font);
            PopStyleColor();
            break;
        case EntryType::Content:
            PushStyleColor(ImGuiCol_Text, GetColorU32(ImGuiCol_Text));
            render_content(entry.text, mono_font);
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

void render_chat_ui(Agent& tab, bool& done) {
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

    // ── Handle compact request ──
    if (ui.compact_requested && !ui.compacting) {
        ui.compacting = true;
        ui.compact_requested = false;
        ui.compact_future = std::async(std::launch::async,
            [&session]() { return session.compact(); });
    }
    if (ui.compacting && ui.compact_future.valid() &&
        ui.compact_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ui.compacting = false;
        auto compact_result = ui.compact_future.get();
        // Clear UI entries and show result
        ui.entries.clear();
        if (compact_result) {
            push_entry(ui, EntryType::Content, "Conversation compacted.", false);
        } else {
            push_entry(ui, EntryType::Content, "Compaction failed: " + compact_result.error(), false);
        }
    }

    // ── Handle clear request ──
    if (ui.clear_requested && !ui.clearing) {
        ui.clearing = true;
        ui.clear_requested = false;
        ui.clear_future = std::async(std::launch::async,
            [&session]() -> Result<void> { session.clear(); return {}; });
    }
    if (ui.clearing && ui.clear_future.valid() &&
        ui.clear_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        ui.clearing = false;
        ui.clear_future.get();
        // Clear UI entries and show confirmation
        ui.entries.clear();
        push_entry(ui, EntryType::Content, "Conversation cleared.", false);
    }

    // ── finalize streaming entry now that all pending data is incorporated ──
    if (stream_ended) {
        finalize_streaming_entry(ui);
        if (!result) {
            push_entry(ui, EntryType::Content, "Error: " + result.error(), false);
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
        if (insert_len > room) insert_len = room;
        memmove(buf.data() + pos + insert_len,
                buf.data() + pos,
                strlen(buf.data()) - pos + 1);
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
    {
        if (!cfg.snippets.empty()) {
            SameLine(0,GetStyle().ItemSpacing.y);
            SetNextItemWidth(GetContentRegionAvail().x); // ~combo_width
            if (BeginCombo("##snippet-ref", "snippets")) {
                for (const auto& [name, content] : cfg.snippets) {
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

    if (!chat.running && IsWindowAppearing()) {
        SetKeyboardFocusHere();
    }

    uint32_t inputFlags = ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap |
        ImGuiInputTextFlags_CallbackAlways;

    ImVec2 inputSize(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4));

    if (InputTextMultiline("##input", buffer.data(), buffer.size(), inputSize, inputFlags, InputTextCallback, &ui.cursor_pos) && !chat.running) {
        string input(trimWhite(buffer.data()));
        if (input.size()) {
            // Push to UI with tags visible (user sees @Page / !Snippet)
            push_entry(ui, EntryType::UserText, input, false);
            // Expand !snippet-name tags before sending to the agent
            string expanded = expand_tags(input);
            start_chat(chat, session, expanded);
            for (auto it = history.begin(); it != history.end();
                it = *it == input ? history.erase(it): ++it);
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
        } else if (ui.compacting) {
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
        ImVec2 pos = GetCursorScreenPos() - ImVec2(0, GetFrameHeight())
            + ImVec2(GetContentRegionAvail().x - GetStyle().FramePadding.x,0);

        pos = pos - ImVec2(branchSize.x,0);
        GetForegroundDrawList()->AddText(pos, ImColor(IM_COL32(255, 180, 50, 255)), branchInfo.c_str());

        pos = pos - ImVec2(sepSize.x,0);
        GetForegroundDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());

        pos = pos - ImVec2(tokenSize.x,0);
        GetForegroundDrawList()->AddText(pos, tokenColor, tokenInfo.c_str());

        pos = pos - ImVec2(sepSize.x,0);
        GetForegroundDrawList()->AddText(pos, GetColorU32(ImGuiCol_TextDisabled), sep.c_str());

        pos = pos - ImVec2(stateSize.x,0);
        GetForegroundDrawList()->AddText(pos, stateColor, stateInfo.c_str());
    }
}

// ── (Group Channel UI removed — agents communicate via send_message / next_message) ──
