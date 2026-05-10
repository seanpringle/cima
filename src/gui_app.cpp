#include "gui_app.h"
#include "gui_chat.h"
#include "jobs.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

extern std::atomic<bool> g_interrupted;

static int next_planner_id = 0;
static int next_builder_id = 0;

static std::string make_tab_title(TabType type) {
    if (type == TabType::Planner) {
        return "Planner #" + std::to_string(++next_planner_id);
    } else {
        return "Builder #" + std::to_string(++next_builder_id);
    }
}

int gui_main(Config cfg) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init error: %s", SDL_GetError());
        return 1;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_RESIZABLE);
    SDL_Window* window = SDL_CreateWindow("llm-chat", 1280, 1280, window_flags);
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

    // ── create the initial Planner tab ──
    std::vector<TabInfo> tabs;
    {
        TabInfo tab;
        tab.id = 0;
        tab.type = TabType::Planner;
        tab.title = make_tab_title(TabType::Planner);
        tab.session = std::make_unique<ChatSession>(cfg, TabType::Planner);
        tab.chat_state = std::make_unique<AsyncChatState>();
        tab.ui_state.mono_font = mono_font;
        tab.ui_state.tab_type = TabType::Planner;
        strncpy(tab.ui_state.title_buf, tab.title.c_str(), sizeof(tab.ui_state.title_buf) - 1);
        strncpy(tab.ui_state.model_buf, tab.session->model().c_str(),
            sizeof(tab.ui_state.model_buf) - 1);

        auto* cs = tab.chat_state.get();
        tab.session->set_output_callback(
            [cs](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        tabs.push_back(std::move(tab));
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

        // If any chat is running and we're quitting, signal interrupt
        if (done) {
            for (auto& tab : tabs) {
                if (tab.chat_state->running) {
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
                if (MenuItem("New Planner Tab")) {
                    TabInfo new_tab;
                    new_tab.id = tabs.empty() ? 0 : tabs.back().id + 1;
                    new_tab.type = TabType::Planner;
                    new_tab.title = make_tab_title(TabType::Planner);
                    new_tab.session =
                        std::make_unique<ChatSession>(cfg, TabType::Planner);
                    new_tab.chat_state = std::make_unique<AsyncChatState>();
                    new_tab.ui_state.mono_font = mono_font;
                    new_tab.ui_state.tab_type = TabType::Planner;
                    strncpy(new_tab.ui_state.title_buf, new_tab.title.c_str(),
                        sizeof(new_tab.ui_state.title_buf) - 1);
                    strncpy(new_tab.ui_state.model_buf,
                        new_tab.session->model().c_str(),
                        sizeof(new_tab.ui_state.model_buf) - 1);
                    new_tab.session->set_output_callback(
                        [chat_state = new_tab.chat_state.get()](const std::string& text, OutputType type) {
                            std::lock_guard<std::mutex> lock(chat_state->mutex);
                            chat_state->pending.emplace_back(text, type);
                        });
                    tabs.push_back(std::move(new_tab));
                }
                if (MenuItem("New Builder Tab")) {
                    TabInfo new_tab;
                    new_tab.id = tabs.empty() ? 0 : tabs.back().id + 1;
                    new_tab.type = TabType::Builder;
                    new_tab.title = make_tab_title(TabType::Builder);
                    new_tab.session =
                        std::make_unique<ChatSession>(cfg, TabType::Builder);
                    new_tab.chat_state = std::make_unique<AsyncChatState>();
                    new_tab.ui_state.mono_font = mono_font;
                    new_tab.ui_state.tab_type = TabType::Builder;
                    strncpy(new_tab.ui_state.title_buf, new_tab.title.c_str(),
                        sizeof(new_tab.ui_state.title_buf) - 1);
                    strncpy(new_tab.ui_state.model_buf,
                        new_tab.session->model().c_str(),
                        sizeof(new_tab.ui_state.model_buf) - 1);
                    new_tab.session->set_output_callback(
                        [chat_state = new_tab.chat_state.get()](const std::string& text, OutputType type) {
                            std::lock_guard<std::mutex> lock(chat_state->mutex);
                            chat_state->pending.emplace_back(text, type);
                        });
                    tabs.push_back(std::move(new_tab));
                }
                Separator();
                if (MenuItem("Exit", "Alt+F4"))
                    done = true;
                EndMenu();
            }
            if (BeginMenu("Jobs")) {
                auto names = JobBoard::instance().list_jobs();
                if (names && !names->empty()) {
                    for (const auto& n : *names) {
                        if (MenuItem(n.c_str())) {
                            // Open job detail window in all tabs
                            for (auto& t : tabs) {
                                t.ui_state.open_job_windows.insert(n);
                            }
                        }
                    }
                } else {
                    Text("No open jobs");
                }
                EndMenu();
            }
            EndMenuBar();
        }

        // ── tab bar ──
        int tab_to_close = -1;
        if (BeginTabBar("##tabs", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_FittingPolicyScroll)) {
            for (int ti = 0; ti < (int)tabs.size(); ti++) {
                auto& tab = tabs[ti];

                std::string label = tab.title;

                bool open = true;
                if (BeginTabItem(label.c_str(), &open,
                        ImGuiTabItemFlags_NoCloseWithMiddleMouseButton)) {
                    // This tab is now active — render its chat UI
                    render_chat_ui(tab, done);
                    EndTabItem();
                }

                if (!open) {
                    tab_to_close = ti;
                }
            }
            EndTabBar();
        }

        End(); // main window

        // ── close tab after iteration (avoid invalidating iterators) ──
        if (tab_to_close >= 0 && tab_to_close < (int)tabs.size()) {
            // Ensure the chat is stopped
            auto& tab = tabs[tab_to_close];
            if (tab.chat_state->running) {
                g_interrupted = true;
                if (tab.chat_state->future.valid()) {
                    tab.chat_state->future.wait();
                    try { tab.chat_state->future.get(); } catch (...) {}
                }
                tab.chat_state->running = false;
                g_interrupted = false;
            }
            tabs.erase(tabs.begin() + tab_to_close);
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
            g_interrupted = true;
            if (tab.chat_state->future.valid()) {
                tab.chat_state->future.wait();
                try { tab.chat_state->future.get(); } catch (...) {}
            }
            tab.chat_state->running = false;
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
