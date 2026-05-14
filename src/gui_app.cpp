#include "gui_app.h"
#include "gui_chat.h"
#include "plan.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <algorithm>
#include <csignal>

int gui_main(Config cfg) {
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

    // Shared group channel across all sessions
    GroupChannel group_channel;

    auto add_tab = [&](const std::string& model_name) {
        TabInfo tab;
        tab.id = next_tab_id++;
        tab.model_name = model_name;
        // Generate a Culture ship name for the tab title
        tab.title = generate_culture_ship_name();
        tab.chat_state = std::make_unique<AsyncChatState>();
        tab.session = std::make_unique<ChatSession>(cfg, &group_channel, tab.chat_state->cancelled);
        tab.session->set_agent_name(tab.title);
        tab.ui_state.mono_font = mono_font;
        tab.session->set_output_callback(
            [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        group_channel.register_agent(tab.id, tab.title);

        tabs.push_back(std::move(tab));
    };

    // Start with one default tab
    add_tab(cfg.model);

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
                group_channel.unregister_agent(tab.id);
                tabs.erase(tabs.begin() + active_tab);
                if (active_tab >= (int)tabs.size())
                    active_tab = (int)tabs.size() - 1;
                if (active_tab < 0)
                    active_tab = 0;
            }
        }

        // Update agent busy status for all tabs
        for (auto& t : tabs) {
            group_channel.set_agent_busy(t.id, t.chat_state->running);
        }

        // ── Poll for group channel notifications and wake idle agents ──
        for (auto& t : tabs) {
            if (t.chat_state->running)
                continue; // agent is busy, notifications will be picked up by inject_usage_notices

            // Check for pending notifications for this agent
            std::vector<PendingNotification> notes;
            {
                notes = group_channel.consume_notifications(t.session->agent_name());
            }

            for (auto& note : notes) {
                // Build an actionable user prompt from the notification
                std::string prompt;
                if (note.from == "user") {
                    prompt = "[Group Channel Message from user]: " + note.summary;
                } else {
                    prompt = "[Group Channel Message from " + note.from + "]: " + note.summary;
                }

                // Start the agent responding to this notification
                if (!t.chat_state->running) {
                    // Ensure any previous future is consumed
                    if (t.chat_state->future.valid()) {
                        try {
                            t.chat_state->future.get();
                        } catch (...) {}
                    }
                    t.chat_state->running = true;
                    *t.chat_state->cancelled = false;
                    t.chat_state->future = std::async(std::launch::async,
                        [session = t.session.get(), cs = t.chat_state.get(),
                         prompt = std::move(prompt)]() {
                            return session->run_once(prompt);
                        });
                }
            }
        }

        // ── Full-width tabs, each with 40% Plan + 60% Chat split inside ──
        {
            // ── Tab bar ──
            if (BeginTabBar("##chat_tabs", ImGuiTabBarFlags_NoTooltip)) {
                for (int ti = 0; ti < (int)tabs.size();) {
                    TabInfo& tab = tabs[ti];
                    bool is_open = true;

                    // Use model name as tab title, or fallback to "Chat"
                    std::string tab_label = tab.title.empty() ? "Chat" : tab.title;

                    // Allow closing via default ImGui tab close button,
                    // but ensure at least one tab remains
                    bool can_close = tabs.size() > 1;

                    PushID(tab.id);
                    ImGuiTabItemFlags tab_flags = ImGuiTabItemFlags_None;
                    if (tab.id == focus_tab_id) {
                        tab_flags |= ImGuiTabItemFlags_SetSelected;
                        focus_tab_id = -1;
                    }

                    if (BeginTabItem((tab_label + "##tab-" + std::to_string(ti)).c_str(),
                            can_close ? &is_open : nullptr,
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
                                if (BeginTabItem("Group")) {
                                    render_group_channel(group_channel, mono_font);
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

                    if (!is_open && can_close) {
                        // Tab was closed via the close button
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
                        // Wait for any outstanding model-fetch to complete before
                        // destroying the tab's ChatUIState / ChatSession
                        if (tab.ui_state.models_future.valid()) {
                            tab.ui_state.models_future.wait();
                        }
                        group_channel.unregister_agent(tab.id);
                        tabs.erase(tabs.begin() + ti);
                        if (active_tab >= (int)tabs.size())
                            active_tab = (int)tabs.size() - 1;
                        if (active_tab < 0)
                            active_tab = 0;
                        continue; // Don't increment ti — we just erased
                    }
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

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
