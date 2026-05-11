#include "gui_app.h"
#include "gui_chat.h"
#include "plan.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

int gui_main(Config cfg) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE);
    SDL_Window* window = SDL_CreateWindow("llm-chat", 1280, 720, window_flags);
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

    auto add_tab = [&](const std::string& model_name) {
        TabInfo tab;
        tab.id = next_tab_id++;
        tab.title = model_name;
        tab.session = std::make_unique<ChatSession>(cfg);
        tab.chat_state = std::make_unique<AsyncChatState>();
        // Wire up the per-tab cancellation token
        tab.session->set_cancelled(tab.chat_state->cancelled);
        tab.ui_state.mono_font = mono_font;
        strncpy(tab.ui_state.title_buf, tab.title.c_str(), sizeof(tab.ui_state.title_buf) - 1);
        strncpy(tab.ui_state.model_buf, tab.session->model().c_str(),
            sizeof(tab.ui_state.model_buf) - 1);
        tab.session->set_output_callback(
            [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });
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
        Begin("llm-chat",
            nullptr,
            ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBringToFrontOnFocus);

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
                        try { tab.chat_state->future.get(); } catch (...) {}
                    }
                    tab.chat_state->running = false;
                }
                tabs.erase(tabs.begin() + active_tab);
                if (active_tab >= (int)tabs.size())
                    active_tab = (int)tabs.size() - 1;
                if (active_tab < 0)
                    active_tab = 0;
            }
        }

        // ── Left panel (40%) with Plan + Right panel (60%) with tabs ──
        {
            // Left panel: Plan document
            BeginChild("##plan_panel", ImVec2(GetContentRegionAvail().x * 0.4f, GetContentRegionAvail().y), true, ImGuiChildFlags_None);
            Text("Plan");
            Separator();
            auto plan_result = PlanBoard::instance().read_plan();
            if (plan_result) {
                if (mono_font)
                    PushFont(mono_font);
                render_content(*plan_result);
                if (mono_font)
                    PopFont();
            } else {
                TextDisabled("(empty plan)");
            }
            EndChild();

            SameLine();

            // Right panel: tabbed chat sessions
            BeginChild("##agent_panel", GetContentRegionAvail(), true, ImGuiChildFlags_None);

            // ── Tab bar ──
            if (BeginTabBar("##chat_tabs", ImGuiTabBarFlags_NoTooltip)) {
                for (int ti = 0; ti < (int)tabs.size(); ) {
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
                    if (BeginTabItem(tab_label.c_str(), can_close ? &is_open : nullptr, tab_flags)) {
                        active_tab = ti;
                        // Render the active chat UI for this tab
                        render_chat_ui(tab, done);
                        EndTabItem();
                    }
                    PopID();

                    if (!is_open && can_close) {
                        // Tab was closed via the close button
                        if (tab.chat_state->running) {
                            *tab.chat_state->cancelled = true;
                            if (tab.chat_state->future.valid()) {
                                tab.chat_state->future.wait();
                                try { tab.chat_state->future.get(); } catch (...) {}
                            }
                            tab.chat_state->running = false;
                        }
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

            EndChild();
        }

        End(); // main window

        // Render the active chat UI overlay (currently unused — raw viewing is
        // now a checkbox toggle in the main chat UI)
        if (active_tab >= 0 && active_tab < (int)tabs.size()) {
            render_chat_overlay(tabs[active_tab], done);
        }

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
                try { tab.chat_state->future.get(); } catch (...) {}
            }
            tab.chat_state->running = false;
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
