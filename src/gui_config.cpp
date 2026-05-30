#include "gui_config.h"
#include "agent.h"
#include "chat.h"
#include "config.h"
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
#include <filesystem>
#include <map>
#include <string>

// ---------------------------------------------------------------------------
// Check whether a binary is available on the system PATH.
// ---------------------------------------------------------------------------
static bool binary_available(const std::string& name) {
    // Quick check common locations first.
    static const std::vector<std::string> prefixes = {"/usr/bin/", "/usr/sbin/", "/usr/local/bin/", "/bin/", "/sbin/"};
    for (const auto& prefix : prefixes) {
        if (std::filesystem::exists(prefix + name))
            return true;
    }
    // Fall back to searching PATH via access().
    const char* path_env = std::getenv("PATH");
    if (!path_env)
        return false;
    std::string path(path_env);
    size_t start = 0;
    while (true) {
        auto colon = path.find(':', start);
        std::string dir = (colon == std::string::npos) ? path.substr(start) : path.substr(start, colon - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (std::filesystem::exists(full))
                return true;
        }
        if (colon == std::string::npos)
            break;
        start = colon + 1;
    }
    return false;
}

using namespace ImGui;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

// ── Shared provider/model/reasoning-effort helper ────────────────
// Used by render_model_tab() for both Primary and SubAgent tabs.
static void render_provider_model_ui(Agent& tab, ChatSession& session) {
    // ── Provider combo ──
    {
        string label = tab.provider_name.empty() ? tab.cfg_->providers[0].name : tab.provider_name;
        if (BeginCombo("Provider", label.c_str())) {
            for (const auto& p : tab.cfg_->providers) {
                bool is_selected = (p.name == tab.provider_name);
                if (Selectable(p.name.c_str(), is_selected)) {
                    if (p.name != tab.provider_name) {
                        // Provider changed — update session client
                        session.set_provider(p);
                        tab.provider_name = p.name;
                        tab.api_type = p.api_type;
                        session.set_api_type(p.api_type);
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

    // ── Model combo (or manual text input if fetch failed) ──

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

    tab.validate_current_model();

    // ── API type combo ──
    {
        std::string current = tab.api_type.empty() ? session.api_type() : tab.api_type;
        if (BeginCombo("API Type", current.c_str())) {
            for (const auto& t : {"openai", "anthropic"}) {
                bool is_selected = (t == current);
                if (Selectable(t, is_selected)) {
                    if (t != current) {
                        tab.api_type = t;
                        session.set_api_type(t);
                    }
                }
                if (is_selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
    }

    // ── Reasoning effort combo ──
    {
        std::string re = tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

        // Find which provider this tab uses so we can get its reasoning_efforts list
        std::vector<std::string> efforts;
        for (const auto& p : tab.cfg_->providers) {
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
}

// ── Horizontal inline provider/model/reasoning selector ──────────
// Used at the top of each agent chat tab.
void render_provider_model_inline(Agent& tab, ChatSession& session) {
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    float width = GetContentRegionAvail().x / 4 - GetStyle().ItemSpacing.x * 2;

    // ── Provider combo ──
    {
        SetNextItemWidth(width);
        string label = tab.provider_name.empty() ? tab.cfg_->providers[0].name : tab.provider_name;
        if (BeginCombo("##inline-provider", label.c_str())) {
            for (const auto& p : tab.cfg_->providers) {
                bool is_selected = (p.name == tab.provider_name);
                if (Selectable(p.name.c_str(), is_selected)) {
                    if (p.name != tab.provider_name) {
                        session.set_provider(p);
                        tab.provider_name = p.name;
                        tab.api_type = p.api_type;
                        session.set_api_type(p.api_type);
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

    // ── Model combo ──
    {
        SameLine(0, GetStyle().ItemSpacing.x);
        SetNextItemWidth(width);
        auto& cache_entry = g_provider_models[tab.provider_name];

        if (!cache_entry.fetched) {
            PushStyleColor(ImGuiCol_Text, IM_COL32(128, 128, 128, 255));
            TextDisabled("Loading...");
            PopStyleColor();
        } else if (!cache_entry.error.empty() || cache_entry.models.empty()) {
            PushStyleColor(ImGuiCol_Text, IM_COL32(255, 100, 100, 255));
            if (!cache_entry.error.empty()) {
                TextDisabled(cache_entry.error.c_str());
            } else {
                TextDisabled("(no models)");
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
            if (BeginCombo("##inline-model", session.model().c_str())) {
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
    }

    tab.validate_current_model();

    // ── API type combo ──
    {
        SameLine(0, GetStyle().ItemSpacing.x);
        SetNextItemWidth(width);
        std::string current = tab.api_type.empty() ? session.api_type() : tab.api_type;
        if (BeginCombo("##inline-apitype", current.c_str())) {
            for (const auto& t : {"openai", "anthropic"}) {
                bool is_selected = (t == current);
                if (Selectable(t, is_selected)) {
                    if (t != current) {
                        tab.api_type = t;
                        session.set_api_type(t);
                    }
                }
                if (is_selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }
    }

    // ── Reasoning effort combo ──
    {
        SameLine(0, GetStyle().ItemSpacing.x);
        SetNextItemWidth(GetContentRegionAvail().x);
        std::string re = tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

        std::vector<std::string> efforts;
        for (const auto& p : tab.cfg_->providers) {
            if (p.name == tab.provider_name) {
                efforts = p.reasoning_efforts;
                break;
            }
        }

        if (BeginCombo("##inline-reasoning", re.empty() ? "(unset)" : re.c_str())) {
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
    Separator();
    PopStyleVar();
}

// ── Helpers for render_config_tab() ───────────────────────────

// ── Compact / Clear / Stop buttons ──
static void render_config_buttons(PrimaryAgent& tab) {
    auto& session = *tab.session;
    Separator();

    // ── Compact button ──
    {
        int ctx_pct = session.context_usage_percent();
        string btn_label = "Compact (" + std::to_string(ctx_pct) + "% context used)";
        if (Button(btn_label.c_str())) {
            tab.ui_state.compact_requested = true;
        }
    }

    // ── Clear Messages button ──
    {
        if (session.conversation().message_count() > 0) {
            int msg_count = static_cast<int>(session.conversation().message_count());
            string btn_label = "Clear Messages (" + std::to_string(msg_count) + ")";
            if (Button(btn_label.c_str())) {
                tab.ui_state.clear_requested = true;
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

// ── Tool Calls sub-tab (tab item must be active) ──
static void render_config_tool_calls_tab(PrimaryAgent& tab) {
    auto& session = *tab.session;
    if (BeginTable("ToolGateTable",
            4,
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        // Column setup
        TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("R/W Sub", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("R/O Sub", ImGuiTableColumnFlags_WidthFixed);
        TableHeadersRow();

        auto names = session.tools_for_testing().tool_names();

        // Categorisation helpers.
        auto category_of = [](const std::string& name) -> const char* {
            if (name == "read_file" || name == "grep_files" || name == "write_file" || name == "edit_file" || name == "find_files")
                return "File";
            if (name == "web_search" || name == "web_fetch")
                return "Web";
            if (name == "run_bwrap" || name == "run_bwrap_ro" || name == "call_subagent")
                return "Execution";
            return "Other";
        };

        // Category display order.
        auto cat_order = [](const char* cat) -> int {
            if (!strcmp(cat, "Execution"))
                return 0;
            if (!strcmp(cat, "File"))
                return 1;
            if (!strcmp(cat, "Web"))
                return 2;
            return 3;
        };

        // Filter and sort.
        names.erase(std::remove_if(names.begin(),
                        names.end(),
                        [](const std::string& name) { return name.rfind("mcp_", 0) == 0 || name == "read_plan" || name == "write_plan"; }),
            names.end());
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            int ca = cat_order(category_of(a));
            int cb = cat_order(category_of(b));
            if (ca != cb)
                return ca < cb;
            return a < b;
        });

        const char* last_cat = nullptr;
        for (const auto& name : names) {
            const char* cat = category_of(name);
            if (cat != last_cat) {
                TableNextRow();
                TableSetBgColor(ImGuiTableBgTarget_RowBg0, GetColorU32(ImGuiCol_TableHeaderBg));
                TableNextColumn();
                TextUnformatted(cat);
                TableNextColumn();
                TableNextColumn();
                TableNextColumn();
                last_cat = cat;
            }

            TableNextRow();

            // Tool name
            TableNextColumn();
            TextUnformatted(name.c_str());
            if (name == "run_bwrap" && !binary_available("bwrap")) {
                SameLine();
                PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                TextUnformatted("(not installed)");
                PopStyleColor();
            }
            if (IsItemHovered()) {
                BeginTooltip();
                TextUnformatted(name.c_str());
                EndTooltip();
            }

            // ── Primary Agent ──
            TableNextColumn();
            {
                bool enabled = session.tool_enabled(name);
                PushID((name + "_primary").c_str());
                if (Checkbox("", &enabled)) {
                    tab.tool_gates[name] = enabled;
                    session.set_tool_enabled(name, enabled);
                }
                PopID();
            }

            // ── Read-write Subagent ──
            // Missing from the map = allowed (same as primary behaviour).
            TableNextColumn();
            {
                if (name == "call_subagent") {
                    bool off = false;
                    BeginDisabled(true);
                    PushID((name + "_rw").c_str());
                    Checkbox("", &off);
                    PopID();
                    EndDisabled();
                } else {
                    auto it = tab.rw_subagent_tool_gates.find(name);
                    bool enabled = (it == tab.rw_subagent_tool_gates.end()) || it->second;
                    PushID((name + "_rw").c_str());
                    if (Checkbox("", &enabled)) {
                        tab.rw_subagent_tool_gates[name] = enabled;
                        for (auto& sa : tab.subagents) {
                            if (!sa.read_only_tools) {
                                sa.session->set_tool_enabled(name, enabled);
                            }
                        }
                    }
                    PopID();
                }
            }

            // ── Read-only Subagent ──
            TableNextColumn();
            {
                if (name == "call_subagent") {
                    bool off = false;
                    BeginDisabled(true);
                    PushID((name + "_ro").c_str());
                    Checkbox("", &off);
                    PopID();
                    EndDisabled();
                } else {
                    bool enabled = tab.ro_subagent_tool_gates[name];
                    PushID((name + "_ro").c_str());
                    if (Checkbox("", &enabled)) {
                        tab.ro_subagent_tool_gates[name] = enabled;
                        for (auto& sa : tab.subagents) {
                            if (sa.read_only_tools) {
                                sa.session->set_tool_enabled(name, enabled);
                            }
                        }
                    }
                    PopID();
                }
            }
        }
        EndTable();
    }
}

// ── MCP Servers sub-tab (tab item must be active) ──
static void render_config_mcp_servers_tab(PrimaryAgent& tab) {
    auto& session = *tab.session;

    // ── Helper: render one row of the MCP server table (gate checkboxes always enabled) ──
    auto render_server_row = [&](const McpEndpoint& mcp, bool is_custom, std::vector<McpEndpoint>::iterator* custom_it = nullptr) {
        bool running = session.mcp_registry().is_running(mcp.name);

        TableNextRow();

        // ── Server name (with tooltip and status indicator) ──
        TableNextColumn();
        PushID(mcp.name.c_str());
        Text("%s", mcp.name.c_str());
        SameLine();
        TextDisabled("(%s)", mcp.transport.c_str());
        if (running) {
            SameLine();
            TextColored(ImVec4(0, 1, 0, 1), "(*)");
        } else if (tab.mcp_error.count(mcp.name)) {
            SameLine();
            TextColored(ImVec4(1, 0, 0, 1), "(!)");
        }
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
            if (!mcp.cwd.empty())
                Text("cwd: %s", mcp.cwd.c_str());
            if (!mcp.api_key.empty())
                Text("api_key: (set)");
            EndTooltip();
        }

        // ── Start/Stop ──
        TableNextColumn();
        bool enabled = tab.mcp_enabled[mcp.name];
        if (Checkbox("##start", &enabled)) {
            tab.mcp_enabled[mcp.name] = enabled;
            tab.mcp_error.erase(mcp.name);
            if (enabled) {
                auto result = is_custom ? session.start_custom_mcp_server(mcp) : session.start_mcp_server(mcp);
                if (!result) {
                    tab.mcp_error[mcp.name] = result.error();
                    tab.mcp_enabled[mcp.name] = false;
                } else {
                    tab.apply_mcp_gates(mcp.name);
                }
            } else {
                for (auto& sa : tab.subagents)
                    sa.session->unregister_mcp_server_tools(mcp.name);
                if (is_custom)
                    session.stop_custom_mcp_server(mcp.name);
                else
                    session.stop_mcp_server(mcp.name);
            }
        }

        // ── Primary gate ──
        TableNextColumn();
        {
            bool p_gate = tab.mcp_server_gates[mcp.name].primary;
            PushID("p");
            if (Checkbox("", &p_gate)) {
                tab.mcp_server_gates[mcp.name].primary = p_gate;
                tab.apply_mcp_gates(mcp.name);
            }
            PopID();
        }

        // ── RW Sub gate ──
        TableNextColumn();
        {
            bool r_gate = tab.mcp_server_gates[mcp.name].rw_sub;
            PushID("r");
            if (Checkbox("", &r_gate)) {
                tab.mcp_server_gates[mcp.name].rw_sub = r_gate;
                tab.apply_mcp_gates(mcp.name);
            }
            PopID();
        }

        // ── RO Sub gate ──
        TableNextColumn();
        {
            bool o_gate = tab.mcp_server_gates[mcp.name].ro_sub;
            PushID("o");
            if (Checkbox("", &o_gate)) {
                tab.mcp_server_gates[mcp.name].ro_sub = o_gate;
                tab.apply_mcp_gates(mcp.name);
            }
            PopID();
        }

        // ── Actions (Edit/X for custom servers only) ──
        TableNextColumn();
        if (is_custom && custom_it) {
            SameLine();
            if (Button("Edit")) {
                tab.mcp_edit.active = true;
                tab.mcp_edit.original_name = mcp.name;
                std::fill(tab.mcp_edit.name_buf.begin(), tab.mcp_edit.name_buf.end(), 0);
                std::fill(tab.mcp_edit.transport_buf.begin(), tab.mcp_edit.transport_buf.end(), 0);
                std::fill(tab.mcp_edit.command_or_url_buf.begin(), tab.mcp_edit.command_or_url_buf.end(), 0);
                std::fill(tab.mcp_edit.args_buf.begin(), tab.mcp_edit.args_buf.end(), 0);
                std::fill(tab.mcp_edit.cwd_buf.begin(), tab.mcp_edit.cwd_buf.end(), 0);
                std::fill(tab.mcp_edit.api_key_buf.begin(), tab.mcp_edit.api_key_buf.end(), 0);
                std::fill(tab.mcp_edit.timeout_buf.begin(), tab.mcp_edit.timeout_buf.end(), 0);
                std::fill(tab.mcp_edit.description_buf.begin(), tab.mcp_edit.description_buf.end(), 0);

                std::copy(mcp.name.begin(), mcp.name.end(), tab.mcp_edit.name_buf.begin());
                std::copy(mcp.transport.begin(), mcp.transport.end(), tab.mcp_edit.transport_buf.begin());
                std::copy(mcp.description.begin(), mcp.description.end(), tab.mcp_edit.description_buf.begin());
                if (mcp.transport == "streamable-http") {
                    std::copy(mcp.url.begin(), mcp.url.end(), tab.mcp_edit.command_or_url_buf.begin());
                } else {
                    std::copy(mcp.command.begin(), mcp.command.end(), tab.mcp_edit.command_or_url_buf.begin());
                }
                if (!mcp.args.empty()) {
                    std::string args_str;
                    for (size_t i = 0; i < mcp.args.size(); ++i) {
                        if (i > 0)
                            args_str += " ";
                        args_str += mcp.args[i];
                    }
                    std::copy(args_str.begin(), args_str.end(), tab.mcp_edit.args_buf.begin());
                }
                if (!mcp.cwd.empty())
                    std::copy(mcp.cwd.begin(), mcp.cwd.end(), tab.mcp_edit.cwd_buf.begin());
                if (!mcp.api_key.empty())
                    std::copy(mcp.api_key.begin(), mcp.api_key.end(), tab.mcp_edit.api_key_buf.begin());
                std::string timeout_str = std::to_string(mcp.timeout_sec);
                std::copy(timeout_str.begin(), timeout_str.end(), tab.mcp_edit.timeout_buf.begin());
                tab.mcp_edit.error.clear();
            }
            SameLine();
            if (Button("X")) {
                std::string name = mcp.name;
                session.stop_custom_mcp_server(name);
                // Erase from vector using the saved iterator
                if (custom_it) {
                    *custom_it = tab.session_.session_data().custom_mcp_servers.erase(*custom_it);
                }
                tab.mcp_enabled.erase(name);
                tab.mcp_server_gates.erase(name);
                tab.mcp_error.erase(name);
                // Note: caller must re-query iterator after erase
            }
        }
        PopID(); // mcp.name
    };

    // ── Combined table: all servers (config + custom) ──
    if (BeginTable("McpServerTable",
            6,
            ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        TableSetupColumn("Server", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("P", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("RW", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("RO", ImGuiTableColumnFlags_WidthFixed);
        TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        TableHeadersRow();

        // ── Config servers (from cima.json) ──
        if (!tab.cfg_->mcp_servers.empty()) {
            TableNextRow();
            TableSetBgColor(ImGuiTableBgTarget_RowBg0, GetColorU32(ImGuiCol_TableHeaderBg));
            TableNextColumn();
            TextUnformatted("From cima.json");
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();

            for (const auto& mcp : tab.cfg_->mcp_servers) {
                render_server_row(mcp, /*is_custom=*/false);
            }
        } else {
            TableNextRow();
            TableNextColumn();
            TextDisabled("No MCP servers in cima.json.");
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
        }

        // ── Custom servers (session-local) ──
        {
            TableNextRow();
            TableSetBgColor(ImGuiTableBgTarget_RowBg0, GetColorU32(ImGuiCol_TableHeaderBg));
            TableNextColumn();
            TextUnformatted("Custom");
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();
            TableNextColumn();

            auto& custom_servers = tab.session_.session_data().custom_mcp_servers;
            if (!custom_servers.empty()) {
                for (auto it = custom_servers.begin(); it != custom_servers.end();) {
                    // Save iterator before potential erase
                    auto current = it;
                    ++it; // advance now so erase doesn't invalidate
                    render_server_row(*current, /*is_custom=*/true, &current);
                    // If render_server_row erased via X button, current iterator is already updated
                }
            } else {
                TableNextRow();
                TableNextColumn();
                TextDisabled("No custom MCP servers.");
                TableNextColumn();
                TableNextColumn();
                TableNextColumn();
                TableNextColumn();
                TableNextColumn();
            }
        }

        EndTable();
    }

    // ── Custom MCP Servers CRUD ──
    auto validate_mcp_name = [](const std::string& name) -> std::string {
        if (name.empty())
            return "Name must not be empty";
        for (char c : name) {
            if (std::isspace(static_cast<unsigned char>(c)))
                return "Name must not contain spaces";
        }
        return {};
    };

    // Check if name conflicts with any config server
    auto is_config_server = [&tab](const std::string& name) -> bool {
        for (const auto& m : tab.cfg_->mcp_servers) {
            if (m.name == name)
                return true;
        }
        return false;
    };

    if (tab.mcp_edit.active) {
        PushID("mcp-edit");

        InputText("Name", tab.mcp_edit.name_buf.data(), tab.mcp_edit.name_buf.size());
        InputText("Description", tab.mcp_edit.description_buf.data(), tab.mcp_edit.description_buf.size());
        if (BeginCombo("Transport", tab.mcp_edit.transport_buf.data())) {
            for (const char* opt : {"stdio", "streamable-http"}) {
                bool selected = strcmp(tab.mcp_edit.transport_buf.data(), opt) == 0;
                if (Selectable(opt, selected)) {
                    std::fill(tab.mcp_edit.transport_buf.begin(), tab.mcp_edit.transport_buf.end(), 0);
                    std::copy(opt, opt + strlen(opt), tab.mcp_edit.transport_buf.begin());
                }
                if (selected)
                    SetItemDefaultFocus();
            }
            EndCombo();
        }

        // Show "Command" or "URL" label based on transport
        std::string ctrl_label = (strcmp(tab.mcp_edit.transport_buf.data(), "streamable-http") == 0) ? "URL" : "Command";
        InputText(ctrl_label.c_str(), tab.mcp_edit.command_or_url_buf.data(), tab.mcp_edit.command_or_url_buf.size());

        // Args and CWD only apply to stdio transport
        bool is_http = strcmp(tab.mcp_edit.transport_buf.data(), "streamable-http") == 0;
        if (!is_http) {
            InputText("Args", tab.mcp_edit.args_buf.data(), tab.mcp_edit.args_buf.size());
            InputText("CWD", tab.mcp_edit.cwd_buf.data(), tab.mcp_edit.cwd_buf.size());
        }

        // API Key shown only for HTTP transport
        if (is_http) {
            InputText("API Key", tab.mcp_edit.api_key_buf.data(), tab.mcp_edit.api_key_buf.size());
        }

        InputText("Timeout", tab.mcp_edit.timeout_buf.data(), tab.mcp_edit.timeout_buf.size());

        if (!tab.mcp_edit.error.empty()) {
            PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
            TextUnformatted(tab.mcp_edit.error.c_str());
            PopStyleColor();
        }
        if (Button("Save")) {
            std::string name(tab.mcp_edit.name_buf.data());
            std::string transport(tab.mcp_edit.transport_buf.data());
            std::string cmd_or_url(tab.mcp_edit.command_or_url_buf.data());
            std::string args_str(tab.mcp_edit.args_buf.data());
            std::string cwd(tab.mcp_edit.cwd_buf.data());
            std::string description(tab.mcp_edit.description_buf.data());
            std::string api_key(tab.mcp_edit.api_key_buf.data());
            std::string timeout_str(tab.mcp_edit.timeout_buf.data());

            std::string err = validate_mcp_name(name);
            if (err.empty() && cmd_or_url.empty())
                err = "Command/URL must not be empty";

            // Check uniqueness against config + existing custom servers
            if (err.empty()) {
                bool name_in_config = is_config_server(name);
                bool name_in_custom = false;
                for (const auto& m : tab.session_.session_data().custom_mcp_servers) {
                    if (m.name == name && name != tab.mcp_edit.original_name) {
                        name_in_custom = true;
                        break;
                    }
                }
                if (name_in_config || name_in_custom)
                    err = "Name already in use";
            }

            // Validate timeout is a positive integer
            if (err.empty() && !timeout_str.empty()) {
                try {
                    int t = std::stoi(timeout_str);
                    if (t <= 0)
                        err = "Timeout must be positive";
                } catch (...) {
                    err = "Timeout must be a valid number";
                }
            }

            if (!err.empty()) {
                tab.mcp_edit.error = std::move(err);
            } else {
                // Parse args string into vector
                std::vector<std::string> args_vec;
                if (!args_str.empty()) {
                    std::stringstream ss(args_str);
                    std::string arg;
                    while (std::getline(ss, arg, ' ')) {
                        if (!arg.empty())
                            args_vec.push_back(arg);
                    }
                }

                int timeout_sec = 60;
                if (!timeout_str.empty()) {
                    try {
                        timeout_sec = std::stoi(timeout_str);
                    } catch (...) {
                    }
                }

                McpEndpoint mcp;
                mcp.name = name;
                mcp.transport = transport;
                mcp.command = cmd_or_url;
                mcp.args = std::move(args_vec);
                mcp.cwd = cwd;
                mcp.description = description;
                if (transport == "streamable-http") {
                    mcp.url = cmd_or_url;
                    mcp.api_key = api_key;
                    mcp.command.clear();
                } else {
                    mcp.command = cmd_or_url;
                }
                mcp.timeout_sec = timeout_sec;

                // Update existing or add new
                if (!tab.mcp_edit.original_name.empty()) {
                    // If renamed, transfer mcp_enabled/mcp_error to the new name
                    // and stop the old server before updating the config.
                    if (name != tab.mcp_edit.original_name) {
                        auto it_en = tab.mcp_enabled.find(tab.mcp_edit.original_name);
                        if (it_en != tab.mcp_enabled.end()) {
                            tab.mcp_enabled[name] = it_en->second;
                            tab.mcp_enabled.erase(it_en);
                        }
                        auto it_g = tab.mcp_server_gates.find(tab.mcp_edit.original_name);
                        if (it_g != tab.mcp_server_gates.end()) {
                            tab.mcp_server_gates[name] = it_g->second;
                            tab.mcp_server_gates.erase(it_g);
                        }
                        tab.mcp_error.erase(tab.mcp_edit.original_name);
                    }
                    session.stop_custom_mcp_server(tab.mcp_edit.original_name);

                    // Find and replace in vector
                    for (auto it = tab.session_.session_data().custom_mcp_servers.begin(); it != tab.session_.session_data().custom_mcp_servers.end();
                        ++it) {
                        if (it->name == tab.mcp_edit.original_name) {
                            *it = std::move(mcp);
                            break;
                        }
                    }
                } else {
                    tab.session_.session_data().custom_mcp_servers.push_back(std::move(mcp));
                }

                // Ensure gates entry exists for this server
                if (!tab.mcp_server_gates.count(name))
                    tab.mcp_server_gates[name] = McpServerGates{};

                // Start the server (find by name — works for both add and edit)
                tab.mcp_enabled[name] = true;
                {
                    bool started = false;
                    for (const auto& s : tab.session_.session_data().custom_mcp_servers) {
                        if (s.name == name) {
                            auto result = session.start_custom_mcp_server(s);
                            if (!result) {
                                tab.mcp_error[name] = result.error();
                                tab.mcp_enabled[name] = false;
                            } else {
                                tab.apply_mcp_gates(name);
                            }
                            started = true;
                            break;
                        }
                    }
                    if (!started) {
                        tab.mcp_error[name] = "Server not found after save";
                        tab.mcp_enabled[name] = false;
                    }
                }

                tab.mcp_edit.active = false;
                tab.mcp_edit.error.clear();
            }
        }
        SameLine();
        if (Button("Cancel")) {
            tab.mcp_edit.active = false;
            tab.mcp_edit.error.clear();
        }
        PopID();
    } else {
        if (Button("+ Add Custom Server")) {
            tab.mcp_edit = {};
            tab.mcp_edit.active = true;
            std::fill(tab.mcp_edit.name_buf.begin(), tab.mcp_edit.name_buf.end(), 0);
            std::fill(tab.mcp_edit.transport_buf.begin(), tab.mcp_edit.transport_buf.end(), 0);
            std::copy("stdio", "stdio" + 5, tab.mcp_edit.transport_buf.begin());
            std::fill(tab.mcp_edit.command_or_url_buf.begin(), tab.mcp_edit.command_or_url_buf.end(), 0);
            std::fill(tab.mcp_edit.args_buf.begin(), tab.mcp_edit.args_buf.end(), 0);
            std::fill(tab.mcp_edit.cwd_buf.begin(), tab.mcp_edit.cwd_buf.end(), 0);
            std::fill(tab.mcp_edit.description_buf.begin(), tab.mcp_edit.description_buf.end(), 0);
            std::fill(tab.mcp_edit.api_key_buf.begin(), tab.mcp_edit.api_key_buf.end(), 0);
            std::fill(tab.mcp_edit.timeout_buf.begin(), tab.mcp_edit.timeout_buf.end(), 0);
            std::copy("60", "60" + 2, tab.mcp_edit.timeout_buf.begin());
        }
    }
}

// ── Snippets sub-tab (tab item must be active) ──
static void render_config_snippets_tab(PrimaryAgent& tab) {
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

    if (tab.snippet_edit.active) {
        PushID("snippet-edit");
        InputText("Name", tab.snippet_edit.name_buf.data(), tab.snippet_edit.name_buf.size());
        InputText("Content", tab.snippet_edit.content_buf.data(), tab.snippet_edit.content_buf.size());
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
                if (!tab.snippet_edit.original_name.empty())
                    tab.session_.session_data().snippets.erase(tab.snippet_edit.original_name);
                tab.session_.session_data().snippets[name] = std::string(tab.snippet_edit.content_buf.data());
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
            std::fill(tab.snippet_edit.name_buf.begin(), tab.snippet_edit.name_buf.end(), 0);
            std::fill(tab.snippet_edit.content_buf.begin(), tab.snippet_edit.content_buf.end(), 0);
        }
    }

    for (auto it = tab.session_.session_data().snippets.begin(); it != tab.session_.session_data().snippets.end();) {
        PushID(it->first.c_str());
        if (Button("X")) {
            it = tab.session_.session_data().snippets.erase(it);
            PopID();
            continue;
        }
        SameLine();
        std::string preview = it->second.substr(0, 60);
        if (it->second.size() > 60)
            preview += "\xe2\x80\xa6";
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

// ── Commands sub-tab (tab item must be active) ──
static void render_config_commands_tab(PrimaryAgent& tab) {
    auto validate_cmd_name = [&tab](const std::string& name) -> std::string {
        if (name.empty())
            return "Name must not be empty";
        for (char c : name) {
            if (std::isspace(static_cast<unsigned char>(c)))
                return "Name must not contain spaces";
        }
        // Check uniqueness (skip when editing the same name)
        if (name != tab.command_edit.original_name && tab.session_.session_data().commands.count(name)) {
            return "A command with this name already exists";
        }
        return {};
    };

    if (tab.command_edit.active) {
        PushID("command-edit");
        InputText("Name", tab.command_edit.name_buf.data(), tab.command_edit.name_buf.size());
        InputText("Command", tab.command_edit.cmd_buf.data(), tab.command_edit.cmd_buf.size());
        if (!tab.command_edit.error.empty()) {
            PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
            TextUnformatted(tab.command_edit.error.data());
            PopStyleColor();
        }
        if (Button("Save")) {
            std::string name(tab.command_edit.name_buf.data());
            std::string err = validate_cmd_name(name);
            if (!err.empty()) {
                tab.command_edit.error = std::move(err);
            } else {
                if (!tab.command_edit.original_name.empty())
                    tab.session_.session_data().commands.erase(tab.command_edit.original_name);
                CommandDef cmd;
                cmd.name = name;
                cmd.command = std::string(tab.command_edit.cmd_buf.data());
                tab.session_.session_data().commands[name] = std::move(cmd);
                tab.command_edit.active = false;
                tab.command_edit.error.clear();
                // Live re-register command tools
                tab.session->refresh_command_tools();
            }
        }
        SameLine();
        if (Button("Cancel")) {
            tab.command_edit.active = false;
            tab.command_edit.error.clear();
        }
        PopID();
    } else {
        if (Button("+ Add Command")) {
            tab.command_edit = {};
            tab.command_edit.active = true;
            std::fill(tab.command_edit.name_buf.begin(), tab.command_edit.name_buf.end(), 0);
            std::fill(tab.command_edit.cmd_buf.begin(), tab.command_edit.cmd_buf.end(), 0);
        }
        SameLine();
        TextDisabled("Commands run outside the bwrap sandbox with full user permissions.");
    }

    for (auto it = tab.session_.session_data().commands.begin(); it != tab.session_.session_data().commands.end();) {
        PushID(it->first.c_str());
        if (Button("X")) {
            it = tab.session_.session_data().commands.erase(it);
            // Live re-register command tools after delete
            tab.session->refresh_command_tools();
            PopID();
            continue;
        }
        SameLine();
        std::string cmd_preview = it->second.command.substr(0, 80);
        if (it->second.command.size() > 80)
            cmd_preview += "\xe2\x80\xa6";
        Text("%s: %s", it->first.c_str(), cmd_preview.c_str());
        if (IsItemHovered()) {
            BeginTooltip();
            Text("Command: %s", it->second.command.c_str());
            EndTooltip();
        }
        SameLine();
        if (Button("Edit")) {
            tab.command_edit.active = true;
            tab.command_edit.original_name = it->first;
            std::fill(tab.command_edit.name_buf.begin(), tab.command_edit.name_buf.end(), 0);
            std::fill(tab.command_edit.cmd_buf.begin(), tab.command_edit.cmd_buf.end(), 0);
            std::copy(it->first.begin(), it->first.end(), tab.command_edit.name_buf.begin());
            std::copy(it->second.command.begin(), it->second.command.end(), tab.command_edit.cmd_buf.begin());
            tab.command_edit.error.clear();
        }
        ++it;
        PopID();
    }
}

// ── Knobs sub-tab (tab item must be active) ──
static void render_config_knobs_tab(PrimaryAgent& tab) {
    TextUnformatted("Per-session knob overrides (0 = use code default).");
    TextUnformatted("Changes take effect immediately on this session.");
    Separator();

    auto& knobs = tab.session_.session_data();

    bool changed = false;

    auto knob_int = [&](const char* label, int& sd_field, int code_default) {
        int display = sd_field > 0 ? sd_field : code_default;
        PushID(label);
        if (InputInt(label, &display)) {
            if (display < 0)
                display = 0;
            sd_field = (display == code_default) ? 0 : display;
            changed = true;
        }
        PopID();
        if (sd_field == 0) {
            SameLine();
            TextDisabled("(default: %d)", code_default);
        }
    };

    knob_int("Max Tool Iterations", knobs.max_tool_iterations, kDefaultMaxToolIterations);
    knob_int("Subagent Timeout (s)", knobs.subagent_timeout, kDefaultSubagentTimeout);
    knob_int("Bash Timeout (s)", knobs.bash_timeout, kDefaultBashTimeout);
    knob_int("Grep Timeout (s)", knobs.grep_timeout, kDefaultGrepTimeout);
    knob_int("Web Search Timeout (s)", knobs.web_search_timeout, kDefaultWebSearchTimeout);
    knob_int("Web Fetch Timeout (s)", knobs.web_fetch_timeout, kDefaultWebFetchTimeout);

    if (changed) {
        tab.session_.apply_knobs_to(*tab.session);
    }
}

// ── Prompt sub-tab (tab item must be active) ──
static void render_config_prompt_tab(PrimaryAgent& tab) {
    // Helper: render one prompt section (label + unique id + read-only multiline)
    auto render_section = [](const char* label, const char* id, std::string text) {
        TextUnformatted(label);
        SameLine();
        TextDisabled("(read-only, for debugging)");
        BeginDisabled(true);
        InputTextMultiline(id, text.data(), text.size(),
            ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 20), ImGuiInputTextFlags_ReadOnly);
        EndDisabled();
    };

    // ── Primary Agent ──
    render_section("Primary Agent: Effective System Prompt", "##prompt_primary", tab.session->effective_prompt());

    Separator();

    // ── Read-write Subagent ──
    for (const auto& sa : tab.subagents) {
        if (!sa.read_only_tools) {
            render_section("Read-write Subagent: Effective System Prompt", "##prompt_rw", sa.session->effective_prompt());
            break;
        }
    }

    Separator();

    // ── Read-only Subagent ──
    for (const auto& sa : tab.subagents) {
        if (sa.read_only_tools) {
            render_section("Read-only Subagent: Effective System Prompt", "##prompt_ro", sa.session->effective_prompt());
            break;
        }
    }
}

void render_config_tab(PrimaryAgent& tab) {
    render_config_buttons(tab);

    // ── Sub-tab bar ──
    Separator();
    if (BeginTabBar("ConfigSubTabs")) {

        if (BeginTabItem("  Tool Calls  ")) {
            render_config_tool_calls_tab(tab);
            EndTabItem();
        }

        if (BeginTabItem("  MCP Servers  ")) {
            render_config_mcp_servers_tab(tab);
            EndTabItem();
        }

        if (BeginTabItem("  Commands  ")) {
            render_config_commands_tab(tab);
            EndTabItem();
        }

        if (BeginTabItem("  Snippets  ")) {
            render_config_snippets_tab(tab);
            EndTabItem();
        }

        if (BeginTabItem("  Knobs  ")) {
            render_config_knobs_tab(tab);
            EndTabItem();
        }

        if (BeginTabItem("  Prompt  ")) {
            render_config_prompt_tab(tab);
            EndTabItem();
        }

        EndTabBar();
    }
}
