#include "gui_app.h"
#include "gui_chat.h"
#include "gui_wiki.h"
#include "plan.h"
#include "wiki.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <algorithm>
#include <csignal>

int gui_main(Config cfg, const std::string& session_name, bool force) {
    // ── App session ──
    auto app_session = std::make_unique<AppSession>(session_name, force);
    app_session->print_welcome();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE);
    SDL_Window* window = SDL_CreateWindow("cima", 1280, 720, window_flags);
    if (!window) {
        SDL_Log("SDL_CreateWindow error: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

    SDL_Renderer* renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer error: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    ImGui::StyleColorsDark();

    ImFont* mono_font = nullptr;
    {
        float display_scale = SDL_GetWindowDisplayScale(window);
        if (display_scale <= 0.0f)
            display_scale = 1.0f;

        float scale = display_scale;
        ImGui::GetStyle().ScaleAllSizes(scale);

        static const ImWchar latin[] = {
            0x0020,
            0x00ff,
            0,
        };
        static const ImWchar unicode[] = {
            0x00f1,
            0x1ffff,
            0,
        };

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFontConfig merge;
        merge.OversampleH = 2;
        merge.MergeMode = true;

        float fs = 18.0f * scale;
        ImGui::GetIO().Fonts->Clear();
        ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSans-Regular.ttf", fs, &cfg, latin);
        ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSans-Regular.ttf", fs, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansMath-Regular.ttf", fs, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansSymbols-Regular.ttf", fs, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansSymbols2-Regular.ttf", fs, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoEmoji-Regular.ttf", fs, &merge, unicode);

        mono_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/DejaVuSansMono.ttf", fs * 0.9f, &cfg, latin);
        if (!mono_font) {
            SDL_Log("Failed to load mono font: font/DejaVuSansMono.ttf");
            return 1;
        }
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/DejaVuSansMono.ttf", fs * 0.9f, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansMath-Regular.ttf", fs * 0.9f, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansSymbols-Regular.ttf", fs * 0.9f, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoSansSymbols2-Regular.ttf", fs * 0.9f, &merge, unicode);
        ImGui::GetIO().Fonts->AddFontFromFileTTF(
            "font/NotoEmoji-Regular.ttf", fs * 0.9f, &merge, unicode);
    }

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    // ── dynamic tab management ──
    std::vector<TabInfo> tabs;
    int next_tab_id = 0;
    int active_tab = 0;
    int focus_tab_id = -1;

    // Shared wiki across all sessions — file-backed via AppSession
    Wiki wiki(app_session->wiki_db_path());

    auto add_tab = [&](const std::string& model_name, const std::string& db_filename = {}) {
        TabInfo tab;
        tab.id = next_tab_id++;
        tab.model_name = model_name;
        // Generate a Culture ship name for the tab title
        tab.title = generate_lotr_name();
        tab.chat_state = std::make_unique<AsyncChatState>();
        tab.session = std::make_unique<ChatSession>(cfg, tab.chat_state->cancelled);
        tab.session->set_agent_name(tab.title);
        tab.ui_state.mono_font = mono_font;
        tab.session->set_output_callback(
            [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        tab.session->set_wiki(&wiki);

        // Set up session DB persistence via AppSession
        {
            std::string agent_filename = db_filename.empty()
                ? tab.title + ".db"
                : db_filename;
            std::string db_path = app_session->agent_db_path(agent_filename);
            tab.session->session_db().set_auto_save_path(db_path);
            tab.session->session_db().load_from_file(db_path);
            // Restore last token usage from metadata so the UI shows
            // the last known count instead of 0 on session resume.
            tab.session->restore_last_usage_from_db();
            // Register with app session for manifest tracking
            app_session->add_agent_db(agent_filename);
        }

        // Load chat UI history from the append-only log (separate from
        // SessionDB so the agent can compact messages without losing history).
        {
            std::string agent_filename = db_filename.empty()
                ? tab.title + ".db"
                : db_filename;
            std::string log_path = app_session->agent_db_path(agent_filename) + ".log";
            tab.ui_state.load_chat_log(log_path);
        }

        // Load plan file (plan + comments persisted across sessions).
        {
            std::string agent_filename = db_filename.empty()
                ? tab.title + ".db"
                : db_filename;
            std::string plan_path = app_session->agent_db_path(agent_filename) + ".plan.json";
            tab.session->plan().load_from_file(plan_path);
        }

        tabs.push_back(std::move(tab));
    };

    // Start tabs based on session state
    if (!app_session->is_new_session()) {
        // Resume existing session: create a tab for each agent DB
        auto agent_dbs = app_session->agent_db_filenames();
        if (agent_dbs.empty()) {
            // No agents in manifest — create one default tab
            add_tab(cfg.model);
        } else {
            for (const auto& db_name : agent_dbs) {
                // Derive agent name from filename: "Gandalf.db" -> "Gandalf"
                std::string agent_name = db_name;
                if (agent_name.size() >= 3 &&
                    agent_name.substr(agent_name.size() - 3) == ".db") {
                    agent_name = agent_name.substr(0, agent_name.size() - 3);
                }
                add_tab(cfg.model, db_name);
                // Override the auto-generated LOTR name with the stored one
                tabs.back().title = agent_name;
                tabs.back().session->set_agent_name(agent_name);
            }
        }
    } else {
        // New session: start with one default tab (AppSession created agent DB already)
        auto dbs = app_session->agent_db_filenames();
        std::string agent_filename = dbs.empty() ? "" : dbs.front();
        add_tab(cfg.model, agent_filename);
    }

    // Focus the first chat tab
    if (!tabs.empty()) {
        focus_tab_id = tabs.back().id;
    }

    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
                done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                done = true;
        }

        // If any chat is running and we're quitting, signal interrupt on all tabs
        if (done) {
            for (auto& tab : tabs) {
                if (tab.chat_state->running) {
                    *tab.chat_state->cancelled = true;
                }
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // ── main window ──
        SetNextWindowPos(ImVec2(0, 0));
        SetNextWindowSize(GetIO().DisplaySize);
        Begin("cima",
            nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // ── Global keyboard shortcuts ──
        // Ctrl+T: open a new tab and switch to it
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, ImGuiInputFlags_RouteGlobal)) {
            focus_tab_id = next_tab_id;
            add_tab(cfg.model);
        }
        // Ctrl+W: close the active tab
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_W, ImGuiInputFlags_RouteGlobal)) {
            if (tabs.size() > 1 && active_tab >= 0 && active_tab < (int)tabs.size()) {
                auto& tab = tabs[active_tab];
                if (tab.chat_state->running) {
                    *tab.chat_state->cancelled = true;
                    if (tab.chat_state->future.valid()) {
                        tab.chat_state->future.wait();
                        try {
                            tab.chat_state->future.get();
                        } catch (...) {
                        }
                    }
                    tab.chat_state->running = false;
                }
                // Wait for any outstanding model-fetch to
                // complete before destroying the tab's ChatUIState / ChatSession
                if (tab.ui_state.models_future.valid()) {
                    tab.ui_state.models_future.wait();
                }

                // Remove from AppSession manifest and delete files
                {
                    std::string agent_filename = tab.title + ".db";
                    app_session->remove_agent_db(agent_filename);

                    // Delete the agent DB and auxiliary files from disk
                    std::error_code ec;
                    std::string db_path = app_session->agent_db_path(agent_filename);
                    std::filesystem::remove(db_path, ec);
                    std::filesystem::remove(db_path + ".log", ec);
                    std::filesystem::remove(db_path + ".plan.json", ec);
                }

                free_lotr_name(tab.title);
                tabs.erase(tabs.begin() + active_tab);
                if (active_tab >= (int)tabs.size())
                    active_tab = (int)tabs.size() - 1;
                if (active_tab < 0)
                    active_tab = 0;
            }
        }

        // ── Full-width tabs, each with 40% Plan + 60% Chat split inside ──
        {
            // ── Tab bar ──
            if (BeginTabBar("##chat_tabs", ImGuiTabBarFlags_NoTooltip)) {
                // ── Wiki tab (read-only, first in the tab bar) ──
                if (BeginTabItem("Wiki")) {
                    auto padding = GetStyle().WindowPadding;
                    auto space = GetContentRegionAvail();

                    SetCursorPos(ImVec2(GetCursorPosX() - padding.x,
                        GetCursorPosY() - GetStyle().ItemSpacing.y));
                    BeginChild("##wiki_view",
                        ImVec2(space.x + padding.x * 2,
                            space.y + padding.y * 2 + GetStyle().ItemSpacing.y),
                        ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

                    render_wiki_tab(wiki, mono_font);

                    EndChild();
                    EndTabItem();
                }

                for (int ti = 0; ti < (int)tabs.size();) {
                    TabInfo& tab = tabs[ti];

                    // Use model name as tab title, or fallback to "Chat"
                    std::string tab_label = tab.title.empty() ? "Chat" : tab.title;

                    PushID(tab.id);
                    ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
                    if (tab.id == focus_tab_id) {
                        tab_flags |= ImGuiTabItemFlags_SetSelected;
                        focus_tab_id = -1;
                    }

                    if (BeginTabItem((tab_label + "##tab-" + std::to_string(ti)).c_str(),
                            nullptr,
                            tab_flags)) {
                        active_tab = ti;

                        auto padding = GetStyle().WindowPadding;
                        auto space = GetContentRegionAvail();

                        SetCursorPos(ImVec2(GetCursorPosX() - padding.x,
                            GetCursorPosY() - GetStyle().ItemSpacing.y));
                        BeginChild("##tab_view",
                            ImVec2(space.x + padding.x * 2,
                                space.y + padding.y * 2 + GetStyle().ItemSpacing.y),
                            ImGuiChildFlags_None,
                            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                                ImGuiWindowFlags_NoScrollWithMouse);

                        // Controls row at tab level (model combo, raw, tokens, branch)
                        render_chat_controls(tab);

                        Separator();

                        // Inside each tab: 40% session tabs (left) + 60% Chat (right)
                        {
                            auto canvas = GetContentRegionAvail();
                            auto tl = GetCursorPos();
                            float gap = GetStyle().ItemSpacing.x * 2.0f;
                            float planWidth = canvas.x * 0.4f - gap;
                            auto planPos = tl;
                            auto planSize = ImVec2(planWidth, canvas.y);
                            auto chatPos = ImVec2(tl.x + planWidth + gap, tl.y);
                            auto chatSize = ImVec2(-1, canvas.y);
                            auto winPos = GetWindowPos();
                            auto sepPosA =
                                ImVec2(winPos.x + tl.x + planWidth + (gap / 2), winPos.y + tl.y);
                            auto sepPosB = ImVec2(sepPosA.x, winPos.y + tl.y + canvas.y);

                            // Left panel: session tabs
                            SetCursorPos(planPos);
                            BeginChild("##left-session-tabs",
                                planSize,
                                ImGuiChildFlags_None,
                                ImGuiWindowFlags_None);

                            if (BeginTabBar("##session-tabs")) {
                                if (BeginTabItem("Plan")) {
                                    auto plan_result = tab.session->plan().read_plan();
                                    if (plan_result) {
                                        PushFont(mono_font);
                                        render_content(*plan_result);
                                        PopFont();
                                    } else {
                                        TextDisabled("(empty plan)");
                                    }
                                    EndTabItem();
                                }
                                if (BeginTabItem("Database")) {
                                    render_session_db_view(tab.session->session_db());
                                    EndTabItem();
                                }
                                EndTabBar();
                            }

                            EndChild();

                            GetWindowDrawList()->AddLine(sepPosA,
                                sepPosB,
                                GetColorU32(ImGuiCol_Separator),
                                GetStyle().ChildBorderSize);

                            // Right panel: Chat UI
                            PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
                            SetCursorPos(chatPos);
                            BeginChild("##tab_chat",
                                chatSize,
                                ImGuiChildFlags_None,
                                ImGuiWindowFlags_None);
                            PopStyleVar();
                            render_chat_ui(tab, done);
                            EndChild();
                        }

                        EndChild();
                        EndTabItem();
                    }
                    PopID();

                    ti++;
                }

                EndTabBar();
            }
        }

        End(); // main window

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // ── clean up all tabs ──
    for (auto& tab : tabs) {
        if (tab.chat_state->running) {
            *tab.chat_state->cancelled = true;
            if (tab.chat_state->future.valid()) {
                tab.chat_state->future.wait();
                try {
                    tab.chat_state->future.get();
                } catch (...) {
                }
            }
            tab.chat_state->running = false;
        }
        // Wait for any outstanding model-fetch to complete before destroying
        // the tab's ChatUIState / ChatSession (use-after-free prevention)
        if (tab.ui_state.models_future.valid()) {
            tab.ui_state.models_future.wait();
        }
    }

    // ── Save all plan files ──
    for (auto& tab : tabs) {
        tab.session->plan().save();
    }

    // ── Save final AppSession manifest ──
    {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) {
            app_session->set_last_cwd(cwd.string());
        }
        app_session->save_manifest();
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
