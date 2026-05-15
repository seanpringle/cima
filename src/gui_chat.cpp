#include "gui_chat.h"
#include "client.h"
#include "notes.h"
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
// #include <fstream>  // no longer needed — persistence via AssistantData
#include <future>
#include <map>
#include <md4c.h>
#include <string>
#include <thread>
#include <unordered_map>

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

void render_config_tab(TabInfo& tab, const Config& cfg, ImFont* mono_font) {
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

        std::packaged_task<void()> task([&ui, api_base, api_key]() {
            ChatClient client(api_base, api_key);
            auto result = client.fetch_models();
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
    Text("Provider:");
    SameLine();
    PushFont(mono_font);
    SetNextItemWidth(-1);

    // Build combo label
    string provider_label = tab.provider_name.empty() ? cfg.providers[0].name : tab.provider_name;

    if (BeginCombo("##provider-combo", provider_label.c_str())) {
        for (const auto& p : cfg.providers) {
            bool is_selected = (p.name == tab.provider_name);
            if (Selectable(p.name.c_str(), is_selected)) {
                if (p.name != tab.provider_name) {
                    // Provider changed — update tab and re-fetch models
                    tab.provider_name = p.name;
                    tab.model_name = p.model;
                    tab.reasoning_effort = p.reasoning_effort;
                    session.set_model(p.model);
                    session.set_reasoning_effort(p.reasoning_effort);
                    trigger_model_fetch();
                }
            }
            if (is_selected) SetItemDefaultFocus();
        }
        EndCombo();
    }
    PopFont();

    // ── Model combo (or manual text input if fetch failed) ──
    Text("Model:");
    SameLine();
    PushFont(mono_font);
    SetNextItemWidth(-1);

    if (!ui.models_fetched->load(std::memory_order_acquire)) {
        // Still loading
        string loading_label = "Loading models...";
        PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
        TextUnformatted(loading_label.c_str());
        PopStyleColor();
    } else if (!ui.models_error.empty() || ui.available_models.empty()) {
        // Fetch failed or returned empty — show manual text input
        // Use a fixed buffer for the model name
        static std::string manual_model;
        manual_model = session.model();
        char buf[256];
        strncpy(buf, manual_model.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
        if (!ui.models_error.empty()) {
            TextUnformatted(("Error: " + ui.models_error).c_str());
        } else {
            TextDisabled("(no models returned)");
        }
        PopStyleColor();
        if (InputText("##model-manual", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string new_model(buf);
            if (!new_model.empty()) {
                session.set_model(new_model);
                tab.model_name = new_model;
            }
        }
    } else {
        // Show dropdown with "default" override first, then discovered models
        string combo_label = session.model();
        if (BeginCombo("##model-combo", combo_label.c_str())) {
            // "default" sends no model field to the API (backend chooses)
            bool is_default = (session.model() == "default");
            if (Selectable("default", is_default)) {
                session.set_model("default");
                tab.model_name = "default";
            }
            if (is_default) SetItemDefaultFocus();

            if (!ui.available_models.empty()) {
                Separator();
                for (const auto& m : ui.available_models) {
                    bool is_selected = (m == session.model());
                    if (Selectable(m.c_str(), is_selected)) {
                        session.set_model(m);
                        tab.model_name = m;
                    }
                    if (is_selected) SetItemDefaultFocus();
                }
            }
            EndCombo();
        }
    }
    PopFont();

    // Validate current model selection (auto-select first if current not found)
    // "default" is always valid and should not be auto-replaced.
    if (ui.models_fetched->load(std::memory_order_acquire) && !ui.models_validated &&
        !ui.available_models.empty()) {
        ui.models_validated = true;
        const auto& current = session.model();
        if (current != "default") {
            bool found = std::any_of(ui.available_models.begin(), ui.available_models.end(),
                [&current](const std::string& m) { return m == current; });
            if (!found) {
                session.set_model(ui.available_models.front());
                tab.model_name = ui.available_models.front();
            }
        }
    }

    Separator();

    // ── Reasoning effort input ──
    Text("Reasoning Effort:");
    SameLine();
    PushFont(mono_font);
    SetNextItemWidth(-1);
    {
        std::string re = tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;
        char buf[128];
        strncpy(buf, re.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if (InputText("##reasoning-effort", buf, sizeof(buf),
                ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string new_re(buf);
            tab.reasoning_effort = new_re;
            session.set_reasoning_effort(new_re);
        }
    }
    PopFont();

    Separator();

    // ── Raw checkbox ──
    Checkbox("Raw", &ui.show_raw);

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
}

// ── Tag expansion for wiki:page-name and !snippet-name references ──
// Expands wiki:pagename to the full wiki page body, and !snippetname to the
// full snippet content.  Non-matching tags are left as-is.
static std::string expand_tags(std::string input, Wiki* wiki,
    const std::map<std::string, std::string>* snippets) {
    if ((!wiki || input.find("wiki:") == std::string::npos) &&
        (!snippets || input.find('!') == std::string::npos))
        return input;

    std::string result;
    size_t i = 0;
    while (i < input.size()) {
        if (wiki && i + 5 <= input.size() && input.substr(i, 5) == "wiki:") {
            size_t start = i + 5;
            size_t end = start;
            while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end])))
                end++;
            std::string name = input.substr(start, end - start);
            if (!name.empty()) {
                auto page = wiki->read_page(name);
                if (page) {
                    result += *page;
                    i = end;
                    continue;
                }
            }
        } else if (snippets && input[i] == '!') {
            size_t start = i + 1;
            size_t end = start;
            while (end < input.size() && !std::isspace(static_cast<unsigned char>(input[end])))
                end++;
            std::string name = input.substr(start, end - start);
            if (!name.empty()) {
                auto it = snippets->find(name);
                if (it != snippets->end()) {
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

    // ── Wiki page reference combo (inserts wiki:pagename tag at cursor) ──
    {
        auto wiki = tab.session->wiki();
        if (wiki) {
            auto pages_result = wiki->list_pages();
            SetNextItemWidth(GetContentRegionAvail().x/2 - GetStyle().ItemSpacing.y/2);
            if (BeginCombo("##wiki-ref", "wiki:")) {
                if (pages_result && !pages_result->empty()) {
                    for (const auto& page : *pages_result) {
                        if (Selectable(page.c_str())) {
                            // Insert "wiki:pagename" tag at cursor position (no trailing space)
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
                }
                EndCombo();
            }
        }
    }

    // ── Snippet reference combo (inserts !snippetname tag at cursor) ──
    {
        const auto* snippets = tab.snippets;
        if (snippets && !snippets->empty()) {
            SameLine(0,GetStyle().ItemSpacing.y);
            SetNextItemWidth(GetContentRegionAvail().x);
            if (BeginCombo("##snippet-ref", "!snippet")) {
                for (const auto& [name, content] : *snippets) {
                    if (Selectable(name.c_str())) {
                        // Insert "!snippetname" tag at cursor position
                        std::string tag = "!" + name;
                        auto& buf = ui.input_buffer;
                        int pos = ui.cursor_pos;
                        if (pos < 0 || (size_t)pos > strlen(buf.data()))
                            pos = (int)strlen(buf.data());
                        size_t room = buf.size() - strlen(buf.data()) - 1;
                        size_t insert_len = tag.size();
                        if (insert_len > room) insert_len = room;
                        memmove(buf.data() + pos + insert_len,
                                buf.data() + pos,
                                strlen(buf.data()) - pos + 1);
                        memcpy(buf.data() + pos, tag.data(), insert_len);
                        ui.cursor_pos = pos + (int)insert_len;
                    }
                }
                EndCombo();
            }
        }
    }

    SetNextItemWidth(GetContentRegionAvail().x);

    PushFont(ui.mono_font);

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

    uint32_t inputFlags = ImGuiInputTextFlags_CtrlEnterForNewLine |
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap |
        ImGuiInputTextFlags_CallbackAlways;

    ImVec2 inputSize(0, std::max(GetFrameHeightWithSpacing() * 3, GetContentRegionAvail().y - 4));

    if (InputTextMultiline("##input", buffer.data(), buffer.size(), inputSize, inputFlags, InputTextCallback, &ui.cursor_pos) && !chat.running) {
        string input(trimWhite(buffer.data()));
        if (input.size()) {
            // Push to UI with tags visible (user sees @Page / !Snippet)
            push_entry(ui, EntryType::UserText, input, false);
            // Expand wiki:page-name and !snippet-name tags before sending to the agent
            string expanded = expand_tags(input, tab.session->wiki(), tab.snippets);
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

        // Token count
        string tokenInfo = std::to_string(session.last_usage().total_tokens) + " tokens";

        // Git branch (refresh if workspace changed)
        {
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

// ===================================================================
// Notes tab (left panel)
// ===================================================================

void render_notes_tab(Notes& notes, ImFont* mono_font) {
    // ── State ──
    static std::string selected_note;
    static char name_buf[256] = {};
    static char body_buf[16384] = {};
    static bool body_dirty = false;
    static bool needs_refresh = false;

    // ── Fetch data ──
    auto names_result = notes.list_all_notes();

    // Validate selected_note still exists
    if (names_result) {
        bool found = false;
        for (const auto& n : *names_result) {
            if (n == selected_note) { found = true; break; }
        }
        if (!found) {
            selected_note.clear();
            body_buf[0] = '\0';
        }
    } else {
        selected_note.clear();
        body_buf[0] = '\0';
    }

    // ── Layout ──
    auto space = GetContentRegionAvail();
    auto tl = GetCursorPos();
    float gap = GetStyle().ItemSpacing.x * 2.0f;
    float left_width = space.x * 0.35f - gap;
    auto win_pos = GetWindowPos();
    auto sep_a = ImVec2(win_pos.x + tl.x + left_width + (gap / 2), win_pos.y + tl.y);
    auto sep_b = ImVec2(sep_a.x, win_pos.y + tl.y + space.y);

    // ── Left panel: note list ──
    SetCursorPos(tl);
    BeginChild("##notes-left", ImVec2(left_width, space.y),
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);

    // "New Note" button
    if (SmallButton("+ New Note")) {
        // Find an unused default name
        std::string base = "Untitled";
        std::string new_name = base;
        int counter = 1;
        if (names_result) {
            while (std::find(names_result->begin(), names_result->end(), new_name) != names_result->end()) {
                new_name = base + " " + std::to_string(counter++);
            }
        }
        notes.write_note(new_name, "");
        selected_note = new_name;
        body_buf[0] = '\0';
        strncpy(name_buf, new_name.c_str(), sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        needs_refresh = true;
    }
    SameLine();
    // "Delete All" button
    if (SmallButton("Delete All")) {
        notes.delete_all_notes();
        selected_note.clear();
        body_buf[0] = '\0';
        needs_refresh = true;
    }

    // Note list
    if (!names_result) {
        TextDisabled("(notes unavailable)");
    } else if (names_result->empty()) {
        TextDisabled("(no notes)");
    } else {
        BeginChild("##notes-scroll");
        for (const auto& name : *names_result) {
            bool is_sel = (name == selected_note);
            if (Selectable(name.c_str(), is_sel)) {
                selected_note = name;
                auto body_result = notes.read_note(name);
                std::string body = body_result ? *body_result : "";
                strncpy(body_buf, body.c_str(), sizeof(body_buf) - 1);
                body_buf[sizeof(body_buf) - 1] = '\0';
                strncpy(name_buf, name.c_str(), sizeof(name_buf) - 1);
                name_buf[sizeof(name_buf) - 1] = '\0';
                body_dirty = false;
            }
            if (is_sel) SetItemDefaultFocus();

            // Delete button for this note
            SameLine();
            PushID(name.c_str());
            if (SmallButton("x")) {
                notes.delete_note(name);
                if (selected_note == name) {
                    selected_note.clear();
                    body_buf[0] = '\0';
                }
                needs_refresh = true;
                PopID();
                break; // list is now stale
            }
            PopID();
        }
        EndChild();
    }

    EndChild();

    // Separator line
    GetWindowDrawList()->AddLine(sep_a, sep_b,
        GetColorU32(ImGuiCol_Separator), GetStyle().ChildBorderSize);

    // ── Right panel: note editor ──
    auto right_pos = ImVec2(tl.x + left_width + gap, tl.y);
    auto right_size = ImVec2(-1, space.y);
    SetCursorPos(right_pos);
    BeginChild("##notes-right", right_size,
        ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);

    if (selected_note.empty()) {
        TextDisabled("Select a note from the left panel");
    } else {
        // Name edit (inline)
        PushItemWidth(GetContentRegionAvail().x);
        if (InputText("##note-name", name_buf, sizeof(name_buf))) {
            // Name changed — rename the note
            std::string new_name(name_buf);
            if (!new_name.empty() && new_name != selected_note) {
                // Write new name, delete old
                notes.write_note(new_name, std::string(body_buf));
                notes.delete_note(selected_note);
                selected_note = new_name;
                needs_refresh = true;
            }
        }
        PopItemWidth();

        Separator();

        // Body editor
        PushFont(mono_font);
        PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 4));
        float body_height = GetContentRegionAvail().y;
        if (InputTextMultiline("##note-body", body_buf, sizeof(body_buf),
                ImVec2(-FLT_MIN, body_height),
                ImGuiInputTextFlags_AllowTabInput)) {
            // Auto-save on every edit
            notes.write_note(selected_note, std::string(body_buf));
            body_dirty = true;
            needs_refresh = true;
        }
        PopStyleVar();
        PopFont();
    }

    EndChild();

    // Reload names if we mutated
    if (needs_refresh) {
        needs_refresh = false;
        // Force refresh next frame by consuming the stale list
        // (the Selectable loop above already broke if we deleted)
    }
}

// ── (Group Channel UI removed — agents communicate via send_message / next_message) ──
