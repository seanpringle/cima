#include "gui_app.h"
#include "app_session.h"
#include "session_data.h"
#include "gui_chat.h"
#include "plan.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <fontconfig/fontconfig.h>

#include <csignal>

// ═══════════════════════════════════════════════════════════════════
// Font lookup helpers
// ═══════════════════════════════════════════════════════════════════

static std::string find_system_font(const std::string& pattern_str) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(pattern_str.c_str()));
    if (!pattern) return {};

    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);

    if (!match || result != FcResultMatch) {
        FcPatternDestroy(match);
        return {};
    }

    FcChar8* file = nullptr;
    if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch) {
        FcPatternDestroy(match);
        return {};
    }

    std::string path = reinterpret_cast<const char*>(file);
    FcPatternDestroy(match);
    return path;
}

static bool find_font_pair(std::string& out_sans, std::string& out_mono) {
    FcInit();

    if (!cfg.font_sans.empty()) out_sans = cfg.font_sans;
    if (!cfg.font_mono.empty()) out_mono = cfg.font_mono;

    auto try_family = [&](const std::string& sans_name, const std::string& mono_name) -> bool {
        if (out_sans.empty()) {
            std::string p = find_system_font(sans_name);
            if (p.empty()) return false;
            out_sans = std::move(p);
        }
        if (out_mono.empty()) {
            std::string p = find_system_font(mono_name);
            if (p.empty()) return false;
            out_mono = std::move(p);
        }
        return true;
    };

    if (try_family("DejaVu Sans", "DejaVu Sans Mono")) return true;
    if (try_family("Liberation Sans", "Liberation Mono")) return true;
    if (try_family("sans-serif", "monospace")) return true;

    return false;
}

// ═══════════════════════════════════════════════════════════════════
// GuiBootstrap — RAII for SDL + ImGui lifecycle
// ═══════════════════════════════════════════════════════════════════

struct GuiBootstrap {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImFont* mono_font = nullptr;
    int result_code = 0;

    GuiBootstrap(const std::string& session_name) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("SDL_Init error: %s", SDL_GetError());
            result_code = 1;
            return;
        }

        std::string window_title = "cima :: " + session_name;
        window = SDL_CreateWindow(window_title.c_str(), 1280, 720,
            SDL_WINDOW_RESIZABLE);
        if (!window) {
            SDL_Log("SDL_CreateWindow error: %s", SDL_GetError());
            result_code = 1;
            return;
        }
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

        renderer = SDL_CreateRenderer(window, NULL);
        if (!renderer) {
            SDL_Log("SDL_CreateRenderer error: %s", SDL_GetError());
            result_code = 1;
            return;
        }

        SDL_SetRenderVSync(renderer, 1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL;
        ImGui::StyleColorsDark();

        // Font setup
        {
            float display_scale = SDL_GetWindowDisplayScale(window);
            if (display_scale <= 0.0f) display_scale = 1.0f;
            float scale = display_scale;
            ImGui::GetStyle().ScaleAllSizes(scale);

            ImFontConfig font_cfg;
            font_cfg.OversampleH = 2;
            float fs = static_cast<float>(cfg.font_size) * scale;

            std::string sans_path, mono_path;
            if (find_font_pair(sans_path, mono_path)) {
                ImGui::GetIO().Fonts->Clear();
                ImGui::GetIO().Fonts->AddFontFromFileTTF(sans_path.c_str(), fs, &font_cfg);
                mono_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
                    mono_path.c_str(), fs, &font_cfg);
            }

            if (!mono_font) {
                SDL_Log("Warning: fontconfig unavailable or no fonts found; "
                        "using ImGui built-in default font");
                mono_font = ImGui::GetIO().Fonts->Fonts[0];
            }
        }

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
    }

    ~GuiBootstrap() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    GuiBootstrap(const GuiBootstrap&) = delete;
    GuiBootstrap& operator=(const GuiBootstrap&) = delete;
};

// ═══════════════════════════════════════════════════════════════════
// Session data helpers
// ═══════════════════════════════════════════════════════════════════

static void restore_tab_from_data(TabInfo& tab, const SessionData& data) {
    if (!data.provider_name.empty()) {
        tab.provider_name = data.provider_name;
        for (const auto& p : cfg.providers) {
            if (p.name == data.provider_name) {
                tab.session->set_provider(p);
                break;
            }
        }
    }

    if (!data.model.empty()) {
        tab.model_name = data.model;
        tab.session->set_model(data.model);
    }

    if (!data.reasoning_effort.empty()) {
        tab.reasoning_effort = data.reasoning_effort;
        tab.session->set_reasoning_effort(data.reasoning_effort);
    }

    tab.session->conversation().from_json(data.conversation);

    tab.ui_state.entries.clear();
    int max_seq = 0;
    if (data.chat_log.is_array()) {
        for (const auto& entry : data.chat_log) {
            DisplayEntry e;
            e.seq = entry.value("seq", 0);
            std::string t = entry.value("type", "Content");
            if (t == "UserText")      e.type = EntryType::UserText;
            else if (t == "Reasoning") e.type = EntryType::Reasoning;
            else if (t == "Content")   e.type = EntryType::Content;
            else if (t == "ToolCall")  e.type = EntryType::ToolCall;
            else continue;
            e.text = entry.value("text", "");
            e.is_streaming = entry.value("streaming", false);
            tab.ui_state.entries.push_back(std::move(e));
            if (e.seq > max_seq) max_seq = e.seq;
        }
    }
    tab.ui_state.next_seq = max_seq + 1;

    if (data.plan.is_object()) {
        tab.session->plan().from_json(data.plan);
    }

    tab.bash_enabled = data.bash_enabled;
    tab.session->set_bash_enabled(data.bash_enabled);
    tab.mcp_enabled = data.mcp_enabled;
}

static SessionData build_session_data(const TabInfo& tab) {
    SessionData data;
    data.provider_name = tab.provider_name;
    data.model = tab.model_name;
    data.reasoning_effort = tab.reasoning_effort;
    data.conversation = tab.session->conversation().to_json();

    json log_arr = json::array();
    for (const auto& e : tab.ui_state.entries) {
        json entry;
        entry["seq"] = e.seq;
        switch (e.type) {
            case EntryType::UserText:  entry["type"] = "UserText"; break;
            case EntryType::Reasoning: entry["type"] = "Reasoning"; break;
            case EntryType::Content:   entry["type"] = "Content"; break;
            case EntryType::ToolCall:  entry["type"] = "ToolCall"; break;
        }
        entry["text"] = e.text;
        if (e.is_streaming) entry["streaming"] = true;
        log_arr.push_back(std::move(entry));
    }
    data.chat_log = std::move(log_arr);
    data.plan = tab.session->plan().to_json();
    data.bash_enabled = tab.bash_enabled;
    data.mcp_enabled = tab.mcp_enabled;

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec) data.last_cwd = cwd.string();
    return data;
}

// ═══════════════════════════════════════════════════════════════════
// Tab creation helpers
// ═══════════════════════════════════════════════════════════════════

static TabInfo create_primary_tab(AppSession& app_session, ImFont* mono_font) {
    TabInfo tab;
    tab.id = 1;
    tab.title = "Assistant";

    if (cfg.providers.empty())
        throw std::runtime_error("No providers configured");

    const auto& provider = cfg.providers[0];
    tab.provider_name = provider.name;
    tab.model_name = provider.model;
    tab.reasoning_effort = provider.reasoning_effort;

    tab.chat_state = std::make_unique<AsyncChatState>();
    tab.session = std::make_unique<ChatSession>(cfg, provider, tab.chat_state->cancelled);
    tab.session->set_agent_name("Assistant");
    tab.ui_state.mono_font = mono_font;
    tab.session->set_output_callback(
        [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
            std::lock_guard<std::mutex> lock(cs->mutex);
            cs->pending.emplace_back(text, type);
        });

    auto session_result = app_session.load_session();
    if (session_result) {
        restore_tab_from_data(tab, *session_result);
    }

    return tab;
}

static std::vector<TabInfo> create_subagent_tabs(PlanBoard& primary_plan, ImFont* mono_font) {
    std::vector<TabInfo> tabs;
    int next_id = 2;

    for (const auto& sa : cfg.subagents) {
        if (cfg.providers.empty())
            throw std::runtime_error("No providers configured");
        const auto& provider = cfg.providers[0];

        TabInfo tab;
        tab.id = next_id++;
        tab.is_subagent = true;
        tab.subagent_name = sa.name;
        tab.title = sa.name;
        tab.read_only_tools = sa.read_only;
        tab.provider_name = provider.name;
        tab.model_name = provider.model;
        tab.reasoning_effort = provider.reasoning_effort;

        tab.chat_state = std::make_unique<AsyncChatState>();
        tab.session = ChatSession::create_subagent(
            cfg, provider, sa.read_only, tab.chat_state->cancelled);
        tab.ui_state.mono_font = mono_font;
        tab.session->set_agent_name(sa.name);
        tab.session->set_output_callback(
            [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        tabs.push_back(std::move(tab));
    }

    return tabs;
}

static void register_subagent_tool(TabInfo& primary, std::vector<TabInfo>& subagent_tabs) {
    primary.session->register_call_subagent_tool(
        /*lookup=*/[&subagent_tabs](const std::string& name) -> ChatSession* {
            for (auto& t : subagent_tabs) {
                if (t.subagent_name == name) return t.session.get();
            }
            return nullptr;
        },
        /*is_running=*/[&subagent_tabs](const std::string& name) -> bool {
            for (auto& t : subagent_tabs) {
                if (t.subagent_name == name) return t.chat_state->running;
            }
            return false;
        },
        /*clear_ui=*/[&subagent_tabs](const std::string& name) -> void {
            for (auto& t : subagent_tabs) {
                if (t.subagent_name == name) {
                    t.ui_state.entries.clear();
                    t.ui_state.next_seq = 1;
                    break;
                }
            }
        },
        /*push_entry=*/[&subagent_tabs](const std::string& name,
                                       const std::string& text) -> void {
            for (auto& t : subagent_tabs) {
                if (t.subagent_name == name) {
                    t.ui_state.entries.push_back(
                        {EntryType::UserText, text, false, t.ui_state.next_seq++});
                    break;
                }
            }
        },
        cfg.subagents);
}

// ═══════════════════════════════════════════════════════════════════
// SDL event handling
// ═══════════════════════════════════════════════════════════════════

/// Poll SDL events. Returns true if the application should quit.
static bool handle_sdl_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT) return true;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return true;
    }
    return false;
}

/// Signal cancellation on all running chats (called on quit).
static void cancel_running_chats(TabInfo& primary, std::vector<TabInfo>& subagents) {
    if (primary.chat_state->running)
        *primary.chat_state->cancelled = true;
    for (auto& t : subagents) {
        if (t.chat_state->running)
            *t.chat_state->cancelled = true;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Frame rendering
// ═══════════════════════════════════════════════════════════════════

static void render_frame(TabInfo& primary, std::vector<TabInfo>& subagent_tabs,
    ImFont* mono_font, bool& done) {
    SetNextWindowPos(ImVec2(0, 0));
    SetNextWindowSize(GetIO().DisplaySize);
    Begin("cima", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse);

    // 40% session tabs (left) + 60% Chat (right)
    {
        auto space = GetContentRegionAvail();
        auto tl = GetCursorPos();
        float gap = GetStyle().ItemSpacing.x * 2.0f;
        float planWidth = space.x * 0.4f - gap;
        auto planPos = tl;
        auto planSize = ImVec2(planWidth, space.y);
        auto chatPos = ImVec2(tl.x + planWidth + gap, tl.y);
        auto chatSize = ImVec2(-1, space.y);
        auto winPos = GetWindowPos();
        auto sepPosA = ImVec2(winPos.x + tl.x + planWidth + (gap / 2), winPos.y + tl.y);
        auto sepPosB = ImVec2(sepPosA.x, winPos.y + tl.y + space.y);

        // Left panel: session tabs
        SetCursorPos(planPos);
        BeginChild("##left-session-tabs", planSize,
            ImGuiChildFlags_None, ImGuiWindowFlags_None);

        if (BeginTabBar("##session-tabs")) {
            if (BeginTabItem("   Plan   ")) {
                auto plan_result = primary.session->plan().read_plan();
                if (plan_result) {
                    render_content(*plan_result, mono_font);
                } else {
                    TextDisabled("(empty plan)");
                }
                EndTabItem();
            }

            if (BeginTabItem("   Config   ")) {
                render_config_tab(primary, cfg, mono_font);
                EndTabItem();
            }

            for (auto& sa_tab : subagent_tabs) {
                PushID(sa_tab.id);
                if (BeginTabItem(("   " + sa_tab.title + "   ##sa-" +
                                  std::to_string(sa_tab.id)).c_str())) {
                    render_subagent_tab(sa_tab, cfg, mono_font);
                    render_subagent_chat(sa_tab, mono_font);
                    EndTabItem();
                }
                PopID();
            }

            EndTabBar();
        }
        EndChild();

        GetWindowDrawList()->AddLine(sepPosA, sepPosB,
            GetColorU32(ImGuiCol_Separator), GetStyle().ChildBorderSize);

        // Right panel: Chat UI
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        SetCursorPos(chatPos);
        BeginChild("##tab_chat", chatSize,
            ImGuiChildFlags_None, ImGuiWindowFlags_None);
        PopStyleVar();
        render_chat_ui(primary, done);
        EndChild();
    }

    End();
}

// ═══════════════════════════════════════════════════════════════════
// Cleanup helpers
// ═══════════════════════════════════════════════════════════════════

static void cancel_and_wait_chat(TabInfo& tab) {
    if (tab.chat_state->running) {
        *tab.chat_state->cancelled = true;
        if (tab.chat_state->future.valid()) {
            tab.chat_state->future.wait();
            try { tab.chat_state->future.get(); } catch (...) {}
        }
        tab.chat_state->running = false;
    }
    if (tab.ui_state.models_future.valid()) {
        tab.ui_state.models_future.wait();
    }
}

static void save_session(AppSession& app_session, TabInfo& primary_tab) {
    auto data = build_session_data(primary_tab);
    app_session.save_session(data);
}

// ═══════════════════════════════════════════════════════════════════
// gui_main — application entry point (orchestrator)
// ═══════════════════════════════════════════════════════════════════

int gui_main(const std::string& session_name, bool force) {
    // Session
    AppSession app_session(session_name, force);
    app_session.print_welcome();

    // SDL + ImGui bootstrap (RAII)
    GuiBootstrap gfx(session_name);
    if (gfx.result_code) return gfx.result_code;

    // Tab creation
    auto primary_tab = create_primary_tab(app_session, gfx.mono_font);
    auto subagent_tabs = create_subagent_tabs(primary_tab.session->plan(), gfx.mono_font);
    register_subagent_tool(primary_tab, subagent_tabs);

    // Main loop
    bool done = false;
    while (!done) {
        if (handle_sdl_events()) {
            cancel_running_chats(primary_tab, subagent_tabs);
            done = true;
            break;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        render_frame(primary_tab, subagent_tabs, gfx.mono_font, done);

        ImGui::Render();
        SDL_SetRenderDrawColor(gfx.renderer, 0, 0, 0, 255);
        SDL_RenderClear(gfx.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), gfx.renderer);
        SDL_RenderPresent(gfx.renderer);
    }

    // Cleanup
    cancel_and_wait_chat(primary_tab);
    for (auto& t : subagent_tabs) cancel_and_wait_chat(t);

    save_session(app_session, primary_tab);

    // GuiBootstrap destructor handles SDL/ImGui shutdown
    return 0;
}
