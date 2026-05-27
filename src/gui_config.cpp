#include "gui_config.h"
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
    static const std::vector<std::string> prefixes = {
        "/usr/bin/", "/usr/sbin/", "/usr/local/bin/", "/bin/", "/sbin/"
    };
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

    // ── Reasoning effort combo ──
    {
        std::string re =
            tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

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

    float width = GetContentRegionAvail().x/3 - GetStyle().ItemSpacing.x*2;

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
        SameLine(0,GetStyle().ItemSpacing.x);
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

    // ── Reasoning effort combo ──
    {
        SameLine(0,GetStyle().ItemSpacing.x);
        SetNextItemWidth(GetContentRegionAvail().x);
        std::string re =
            tab.reasoning_effort.empty() ? session.reasoning_effort() : tab.reasoning_effort;

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

void render_config_tab(PrimaryAgent& tab) {
    auto& ui = tab.ui_state;
    auto& session = *tab.session;

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

        // ── Stop button ──
        {
            BeginDisabled(!tab.chat_state->running);
            if (Button("Stop")) {
                *tab.chat_state->cancelled = true;
            }
            EndDisabled();
        }
    }

    // ── Sub-tab bar ──
    {
        Separator();

        if (BeginTabBar("ConfigSubTabs")) {

            // ── Tool Calls sub-tab ──
            if (BeginTabItem("  Tool Calls  ")) {
                if (BeginTable("ToolGateTable",
                        4,
                        ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH |
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    // Column setup
                    TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch);
                    TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthFixed);
                    TableSetupColumn("R/W Sub", ImGuiTableColumnFlags_WidthFixed);
                    TableSetupColumn("R/O Sub", ImGuiTableColumnFlags_WidthFixed);
                    TableHeadersRow();

                    auto names = session.tools_for_testing().tool_names();

                    // Categorisation helpers.
                    auto category_of = [](const std::string& name) -> const char* {
                        if (name == "read_file" || name == "grep_files" ||
                            name == "write_file" || name == "edit_file" ||
                            name == "find_files")
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
                                    [](const std::string& name) {
                                        return name.rfind("mcp_", 0) == 0 || name == "read_plan" ||
                                            name == "write_plan";
                                    }),
                        names.end());
                    std::sort(names.begin(),
                        names.end(),
                        [&](const std::string& a, const std::string& b) {
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
                            TableSetBgColor(
                                ImGuiTableBgTarget_RowBg0, GetColorU32(ImGuiCol_TableHeaderBg));
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
                                bool enabled =
                                    (it == tab.rw_subagent_tool_gates.end()) || it->second;
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
                EndTabItem();
            }

            // ── MCP Servers sub-tab ──
            if (BeginTabItem("  MCP Servers  ")) {
                if (!tab.cfg_->mcp_servers.empty()) {
                    TextDisabled("Configured servers:");
                    for (const auto& mcp : tab.cfg_->mcp_servers) {
                        bool enabled = tab.mcp_enabled[mcp.name];
                        bool changed = Checkbox(mcp.name.c_str(), &enabled);
                        if (changed) {
                            tab.mcp_enabled[mcp.name] = enabled;
                            tab.mcp_error.erase(mcp.name);
                            if (enabled) {
                                auto result = session.start_mcp_server(mcp);
                                if (!result) {
                                    tab.mcp_error[mcp.name] = result.error();
                                    tab.mcp_enabled[mcp.name] = false;
                                }
                            } else {
                                session.stop_mcp_server(mcp.name);
                            }
                        }
                        SameLine();
                        TextDisabled("(%s)", mcp.transport.c_str());
                        if (session.mcp_registry().is_running(mcp.name)) {
                            SameLine();
                            TextColored(ImVec4(0, 1, 0, 1), "(*) running");
                        } else if (tab.mcp_error.count(mcp.name)) {
                            SameLine();
                            TextColored(
                                ImVec4(1, 0, 0, 1), "(!) %s", tab.mcp_error[mcp.name].c_str());
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
                            EndTooltip();
                        }
                    }
                } else {
                    TextDisabled("No MCP servers in cima.json.");
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
                    InputText("Description",
                        tab.mcp_edit.description_buf.data(),
                        tab.mcp_edit.description_buf.size());
                    if (BeginCombo("Transport", tab.mcp_edit.transport_buf.data())) {
                        for (const char* opt : {"stdio", "streamable-http"}) {
                            bool selected = strcmp(tab.mcp_edit.transport_buf.data(), opt) == 0;
                            if (Selectable(opt, selected)) {
                                std::fill(tab.mcp_edit.transport_buf.begin(),
                                    tab.mcp_edit.transport_buf.end(),
                                    0);
                                std::copy(
                                    opt, opt + strlen(opt), tab.mcp_edit.transport_buf.begin());
                            }
                            if (selected)
                                break;
                        }
                        EndCombo();
                    }

                    // Show "Command" or "URL" label based on transport
                    std::string ctrl_label =
                        (strcmp(tab.mcp_edit.transport_buf.data(), "streamable-http") == 0)
                        ? "URL"
                        : "Command";
                    InputText(ctrl_label.c_str(),
                        tab.mcp_edit.command_or_url_buf.data(),
                        tab.mcp_edit.command_or_url_buf.size());

                    InputText("Args", tab.mcp_edit.args_buf.data(), tab.mcp_edit.args_buf.size());
                    InputText("CWD", tab.mcp_edit.cwd_buf.data(), tab.mcp_edit.cwd_buf.size());

                    // API Key shown only for HTTP transport
                    bool is_http =
                        strcmp(tab.mcp_edit.transport_buf.data(), "streamable-http") == 0;
                    if (is_http) {
                        InputText("API Key",
                            tab.mcp_edit.api_key_buf.data(),
                            tab.mcp_edit.api_key_buf.size());
                    }

                    InputText("Timeout",
                        tab.mcp_edit.timeout_buf.data(),
                        tab.mcp_edit.timeout_buf.size());

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
                                    tab.mcp_error.erase(tab.mcp_edit.original_name);
                                }
                                session.stop_custom_mcp_server(tab.mcp_edit.original_name);

                                // Find and replace in vector
                                for (auto it = tab.session_.session_data().custom_mcp_servers.begin();
                                    it != tab.session_.session_data().custom_mcp_servers.end();
                                    ++it) {
                                    if (it->name == tab.mcp_edit.original_name) {
                                        *it = std::move(mcp);
                                        break;
                                    }
                                }
                            } else {
                                tab.session_.session_data().custom_mcp_servers.push_back(std::move(mcp));
                            }

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
                        std::fill(tab.mcp_edit.transport_buf.begin(),
                            tab.mcp_edit.transport_buf.end(),
                            0);
                        std::copy("stdio", "stdio" + 5, tab.mcp_edit.transport_buf.begin());
                        std::fill(tab.mcp_edit.command_or_url_buf.begin(),
                            tab.mcp_edit.command_or_url_buf.end(),
                            0);
                        std::fill(tab.mcp_edit.args_buf.begin(), tab.mcp_edit.args_buf.end(), 0);
                        std::fill(tab.mcp_edit.cwd_buf.begin(), tab.mcp_edit.cwd_buf.end(), 0);
                        std::fill(tab.mcp_edit.description_buf.begin(),
                            tab.mcp_edit.description_buf.end(),
                            0);
                        std::fill(
                            tab.mcp_edit.api_key_buf.begin(), tab.mcp_edit.api_key_buf.end(), 0);
                        std::fill(
                            tab.mcp_edit.timeout_buf.begin(), tab.mcp_edit.timeout_buf.end(), 0);
                        std::copy("60", "60" + 2, tab.mcp_edit.timeout_buf.begin());
                    }
                }

                // List custom MCP servers
                for (auto it = tab.session_.session_data().custom_mcp_servers.begin();
                    it != tab.session_.session_data().custom_mcp_servers.end();) {
                    PushID(it->name.c_str());
                    bool enabled = tab.mcp_enabled[it->name];
                    if (Checkbox("##en", &enabled)) {
                        tab.mcp_enabled[it->name] = enabled;
                        tab.mcp_error.erase(it->name);
                        if (enabled) {
                            auto result = session.start_custom_mcp_server(*it);
                            if (!result) {
                                tab.mcp_error[it->name] = result.error();
                                tab.mcp_enabled[it->name] = false;
                            }
                        } else {
                            session.stop_custom_mcp_server(it->name);
                        }
                    }
                    SameLine();
                    std::string label = it->name + " (" + it->transport + ")";
                    Text("%s", label.c_str());

                    if (session.mcp_registry().is_running(it->name)) {
                        SameLine();
                        TextColored(ImVec4(0, 1, 0, 1), "(*) running");
                    } else if (tab.mcp_error.count(it->name)) {
                        SameLine();
                        TextColored(ImVec4(1, 0, 0, 1), "(!) %s", tab.mcp_error[it->name].c_str());
                    }

                    if (IsItemHovered()) {
                        BeginTooltip();
                        Text("command: %s", it->command.c_str());
                        if (!it->args.empty()) {
                            std::string args_str;
                            for (const auto& a : it->args) {
                                if (!args_str.empty())
                                    args_str += " ";
                                args_str += a;
                            }
                            Text("args: %s", args_str.c_str());
                        }
                        if (!it->url.empty())
                            Text("url: %s", it->url.c_str());
                        if (!it->cwd.empty())
                            Text("cwd: %s", it->cwd.c_str());
                        if (!it->api_key.empty())
                            Text("api_key: (set)");
                        EndTooltip();
                    }

                    SameLine();
                    if (Button("Edit")) {
                        tab.mcp_edit.active = true;
                        tab.mcp_edit.original_name = it->name;
                        std::fill(tab.mcp_edit.name_buf.begin(), tab.mcp_edit.name_buf.end(), 0);
                        std::fill(tab.mcp_edit.transport_buf.begin(),
                            tab.mcp_edit.transport_buf.end(),
                            0);
                        std::fill(tab.mcp_edit.command_or_url_buf.begin(),
                            tab.mcp_edit.command_or_url_buf.end(),
                            0);
                        std::fill(tab.mcp_edit.args_buf.begin(), tab.mcp_edit.args_buf.end(), 0);
                        std::fill(tab.mcp_edit.cwd_buf.begin(), tab.mcp_edit.cwd_buf.end(), 0);
                        std::fill(
                            tab.mcp_edit.api_key_buf.begin(), tab.mcp_edit.api_key_buf.end(), 0);
                        std::fill(
                            tab.mcp_edit.timeout_buf.begin(), tab.mcp_edit.timeout_buf.end(), 0);
                        std::fill(tab.mcp_edit.description_buf.begin(),
                            tab.mcp_edit.description_buf.end(),
                            0);

                        std::copy(it->name.begin(), it->name.end(), tab.mcp_edit.name_buf.begin());
                        std::copy(it->transport.begin(),
                            it->transport.end(),
                            tab.mcp_edit.transport_buf.begin());
                        std::copy(it->description.begin(),
                            it->description.end(),
                            tab.mcp_edit.description_buf.begin());

                        if (it->transport == "streamable-http") {
                            std::copy(it->url.begin(),
                                it->url.end(),
                                tab.mcp_edit.command_or_url_buf.begin());
                        } else {
                            std::copy(it->command.begin(),
                                it->command.end(),
                                tab.mcp_edit.command_or_url_buf.begin());
                        }

                        // Serialize args back to space-separated string
                        if (!it->args.empty()) {
                            std::string args_str;
                            for (size_t i = 0; i < it->args.size(); ++i) {
                                if (i > 0)
                                    args_str += " ";
                                args_str += it->args[i];
                            }
                            std::copy(
                                args_str.begin(), args_str.end(), tab.mcp_edit.args_buf.begin());
                        }

                        if (!it->cwd.empty()) {
                            std::copy(it->cwd.begin(), it->cwd.end(), tab.mcp_edit.cwd_buf.begin());
                        }
                        if (!it->api_key.empty()) {
                            std::copy(it->api_key.begin(),
                                it->api_key.end(),
                                tab.mcp_edit.api_key_buf.begin());
                        }

                        std::string timeout_str = std::to_string(it->timeout_sec);
                        std::copy(timeout_str.begin(),
                            timeout_str.end(),
                            tab.mcp_edit.timeout_buf.begin());

                        tab.mcp_edit.error.clear();
                    }
                    SameLine();
                    if (Button("X")) {
                        {
                            std::string name = it->name; // save before erase
                            session.stop_custom_mcp_server(name);
                            it = tab.session_.session_data().custom_mcp_servers.erase(it);
                            tab.mcp_enabled.erase(name);
                            tab.mcp_error.erase(name);
                        }
                        PopID();
                        continue;
                    }
                    ++it;
                    PopID();
                }
                EndTabItem();
            }

            // ── Snippets sub-tab ──
            if (BeginTabItem("  Snippets  ")) {
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
                    InputText(
                        "Name", tab.snippet_edit.name_buf.data(), tab.snippet_edit.name_buf.size());
                    InputText("Content",
                        tab.snippet_edit.content_buf.data(),
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
                            if (!tab.snippet_edit.original_name.empty())
                                tab.session_.session_data().snippets.erase(tab.snippet_edit.original_name);
                            tab.session_.session_data().snippets[name] =
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
                        std::fill(
                            tab.snippet_edit.name_buf.begin(), tab.snippet_edit.name_buf.end(), 0);
                        std::fill(tab.snippet_edit.content_buf.begin(),
                            tab.snippet_edit.content_buf.end(),
                            0);
                    }
                }

                for (auto it = tab.session_.session_data().snippets.begin();
                    it != tab.session_.session_data().snippets.end();) {
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
                        std::fill(
                            tab.snippet_edit.name_buf.begin(), tab.snippet_edit.name_buf.end(), 0);
                        std::fill(tab.snippet_edit.content_buf.begin(),
                            tab.snippet_edit.content_buf.end(),
                             0);
                        std::copy(
                            it->first.begin(), it->first.end(), tab.snippet_edit.name_buf.begin());
                        std::copy(it->second.begin(),
                            it->second.end(),
                            tab.snippet_edit.content_buf.begin());
                        tab.snippet_edit.error.clear();
                    }
                    ++it;
                    PopID();
                }
                EndTabItem();
            }

            // ── Knobs sub-tab ──
            if (BeginTabItem("  Knobs  ")) {
                TextUnformatted("Per-session knob overrides (0 = use code default).");
                TextUnformatted("Changes take effect immediately on this session.");
                Separator();

                auto& knobs = tab.session_.session_data();

                bool changed = false;

                auto knob_int = [&](const char* label, int& sd_field, int code_default) {
                    int display = sd_field > 0 ? sd_field : code_default;
                    PushID(label);
                    if (InputInt(label, &display)) {
                        if (display < 0) display = 0;
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

                EndTabItem();
            }

            // ── Prompt sub-tab ──
            if (BeginTabItem("  Prompt  ")) {
                TextUnformatted("Effective System Prompt:");
                SameLine();
                TextDisabled("(read-only, for debugging)");
                {
                    std::string prompt = tab.session->effective_prompt();
                    BeginDisabled(true);
                    InputTextMultiline("##sysprompt",
                        prompt.data(), prompt.size(),
                        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 20),
                        ImGuiInputTextFlags_ReadOnly);
                    EndDisabled();
                }

                Separator();

                TextUnformatted("Tool Definitions (sent with each request):");
                SameLine();
                TextDisabled("(read-only, for debugging)");
                {
                    std::string tools = tab.session->tools_json().dump(2);
                    BeginDisabled(true);
                    PushFont(mono_font);
                    InputTextMultiline("##toolsjson",
                        tools.data(), tools.size(),
                        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 20),
                        ImGuiInputTextFlags_ReadOnly);
                    PopFont();
                    EndDisabled();
                }

                EndTabItem();
            }

            EndTabBar();
        }
    }
}
