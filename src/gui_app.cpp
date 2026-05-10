#include "gui_app.h"
#include "gui_chat.h"
#include "jobs.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

extern std::atomic<bool> g_interrupted;

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

    // ── create the two fixed panels: Planner (left) and Builder (right) ──
    TabInfo planner_tab;
    TabInfo builder_tab;

    // Planner
    planner_tab.id = 0;
    planner_tab.type = TabType::Planner;
    planner_tab.title = "Planner";
    planner_tab.session = std::make_unique<ChatSession>(cfg, TabType::Planner);
    planner_tab.chat_state = std::make_unique<AsyncChatState>();
    planner_tab.ui_state.mono_font = mono_font;
    planner_tab.ui_state.tab_type = TabType::Planner;
    strncpy(planner_tab.ui_state.title_buf, "Planner", sizeof(planner_tab.ui_state.title_buf) - 1);
    strncpy(planner_tab.ui_state.model_buf, planner_tab.session->model().c_str(),
        sizeof(planner_tab.ui_state.model_buf) - 1);
    planner_tab.session->set_output_callback(
        [cs = planner_tab.chat_state.get()](const std::string& text, OutputType type) {
            std::lock_guard<std::mutex> lock(cs->mutex);
            cs->pending.emplace_back(text, type);
        });

    // Builder
    builder_tab.id = 1;
    builder_tab.type = TabType::Builder;
    builder_tab.title = "Builder";
    builder_tab.session = std::make_unique<ChatSession>(cfg, TabType::Builder);
    builder_tab.chat_state = std::make_unique<AsyncChatState>();
    builder_tab.ui_state.mono_font = mono_font;
    builder_tab.ui_state.tab_type = TabType::Builder;
    strncpy(builder_tab.ui_state.title_buf, "Builder", sizeof(builder_tab.ui_state.title_buf) - 1);
    strncpy(builder_tab.ui_state.model_buf, builder_tab.session->model().c_str(),
        sizeof(builder_tab.ui_state.model_buf) - 1);
    builder_tab.session->set_output_callback(
        [cs = builder_tab.chat_state.get()](const std::string& text, OutputType type) {
            std::lock_guard<std::mutex> lock(cs->mutex);
            cs->pending.emplace_back(text, type);
        });

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

        // If any chat is running and we're quitting, signal interrupt
        if (done) {
            for (auto* tab : { &planner_tab, &builder_tab }) {
                if (tab->chat_state->running) {
                    g_interrupted = true;
                    break;
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
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── menu bar ──
        if (BeginMenuBar()) {
            if (BeginMenu("File")) {
                if (MenuItem("Exit", "Alt+F4"))
                    done = true;
                EndMenu();
            }
            if (BeginMenu("Jobs")) {
                auto names = JobBoard::instance().list_jobs();
                if (names && !names->empty()) {
                    for (const auto& n : *names) {
                        if (MenuItem(n.c_str())) {
                            // Open job detail window in both panels
                            planner_tab.ui_state.open_job_windows.insert(n);
                            builder_tab.ui_state.open_job_windows.insert(n);
                        }
                    }
                } else {
                    Text("No open jobs");
                }
                EndMenu();
            }
            EndMenuBar();
        }

        // ── two fixed panels (50:50 horizontal split) ──
        {
            ImVec2 content = GetContentRegionAvail();
            float split = content.x * 0.5f;
            float separator_w = GetStyle().ItemSpacing.x;

            // Left panel (Planner)
            BeginChild("##planner_panel", ImVec2(split - separator_w, content.y), true);
            render_chat_ui(planner_tab, done);
            EndChild();

            SameLine();

            // Vertical separator
            SeparatorEx(ImGuiSeparatorFlags_Vertical);
            SameLine();

            // Right panel (Builder)
            BeginChild("##builder_panel", ImVec2(content.x - split - separator_w * 2, content.y), true);
            render_chat_ui(builder_tab, done);
            EndChild();
        }

        End(); // main window

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // ── clean up both panels ──
    for (auto* tab : { &planner_tab, &builder_tab }) {
        if (tab->chat_state->running) {
            g_interrupted = true;
            if (tab->chat_state->future.valid()) {
                tab->chat_state->future.wait();
                try { tab->chat_state->future.get(); } catch (...) {}
            }
            tab->chat_state->running = false;
        }
    }
    g_interrupted = false;

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
