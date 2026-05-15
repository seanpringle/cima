#include "gui_app.h"
#include "assistant_data.h"
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

// ── Helper: save a single tab to its consolidated JSON file ──
static void save_tab_to_disk(const TabInfo& tab, AppSession& app_session) {
    std::string json_path = app_session.session_file_path(tab.title + ".json");

    AssistantData data;
    data.name = tab.title;
    data.provider_name = tab.provider_name;
    data.model = tab.model_name;
    data.reasoning_effort = tab.reasoning_effort;
    data.workspace_path = tab.session->safe_dir();
    data.conversation = tab.session->conversation().to_json();

    // Serialize chat log entries
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
        if (e.is_streaming) {
            entry["streaming"] = true;
        }
        log_arr.push_back(std::move(entry));
    }
    data.chat_log = std::move(log_arr);

    // Plan data
    data.plan = tab.session->plan().to_json();

    // Notes data
    data.notes = tab.session->notes().to_json();

    data.save_to_file(json_path);
}

// ── Helper: load a tab from its consolidated JSON file ──
static void load_tab_from_disk(TabInfo& tab, AppSession& app_session,
    const std::vector<Provider>& providers) {
    std::string json_path = app_session.session_file_path(tab.title + ".json");

    AssistantData data;
    auto result = data.load_from_file(json_path);
    if (!result) {
        // File doesn't exist or is corrupt — start fresh
        return;
    }

    // Restore provider name (if the provider still exists)
    if (!data.provider_name.empty()) {
        tab.provider_name = data.provider_name;
        // Verify the provider still exists
        bool found = false;
        for (const auto& p : providers) {
            if (p.name == data.provider_name) {
                // Update the session to use this provider's api_base/api_key
                tab.session->set_provider(p);
                found = true;
                break;
            }
        }
        if (!found) {
            // Keep the name anyway (the Config tab's provider combo will show a fallback)
        }
    }

    // Restore model from saved data (but keep the config-provided model if
    // the saved data has an empty model)
    // Note: session.set_provider() above already set the model from the provider.
    // Override with saved model if non-empty (restores the user's last selection).
    if (!data.model.empty()) {
        tab.model_name = data.model;
        tab.session->set_model(data.model);
    }

    // Restore reasoning effort
    if (!data.reasoning_effort.empty()) {
        tab.reasoning_effort = data.reasoning_effort;
        tab.session->set_reasoning_effort(data.reasoning_effort);
    }

    // Restore workspace path (safe directory)
    if (!data.workspace_path.empty()) {
        tab.session->set_safe_dir(data.workspace_path);
    }

    // Restore conversation
    tab.session->conversation().from_json(data.conversation);

    // Restore chat log
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

    // Restore plan
    if (data.plan.is_object()) {
        tab.session->plan().from_json(data.plan);
    }

    // Restore notes
    if (data.notes.is_object()) {
        tab.session->notes().from_json(data.notes);
    }
}

// ── Helper: find a provider index by name, fallback to 0 ──
static int find_provider_index(const std::vector<Provider>& providers,
    const std::string& name) {
    if (name.empty()) return 0;
    for (size_t i = 0; i < providers.size(); i++) {
        if (providers[i].name == name) return (int)i;
    }
    return 0; // fallback to first
}

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
    Wiki wiki(app_session->wiki_dir_path());

    // Save a reference to cfg.providers for lambda capture (lifetime: gui_main)
    const auto& providers = cfg.providers;

    auto add_tab = [&](int provider_idx, const std::string& restore_title = {}) {
        const auto& provider = providers[provider_idx];

        TabInfo tab;
        tab.id = next_tab_id++;
        tab.provider_name = provider.name;
        tab.model_name = provider.model;
        tab.reasoning_effort = provider.reasoning_effort;

        if (!restore_title.empty()) {
            tab.title = restore_title;
            reserve_lotr_name(tab.title);
        } else {
            tab.title = generate_lotr_name();
        }

        tab.chat_state = std::make_unique<AsyncChatState>();
        tab.session = std::make_unique<ChatSession>(cfg, provider, tab.chat_state->cancelled);
        tab.session->set_agent_name(tab.title);
        tab.ui_state.mono_font = mono_font;
        tab.session->set_output_callback(
            [cs = tab.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        tab.session->set_wiki(&wiki);

        // Point to shared config snippets (cima.json)
        tab.snippets = &cfg.snippets;

        // Load consolidated assistant data from <title>.json
        load_tab_from_disk(tab, *app_session, providers);

        tabs.push_back(std::move(tab));
    };

    // Restore existing tabs from session manifest
    {
        const auto& assistant_files = app_session->assistant_files();
        if (!assistant_files.empty()) {
            for (const auto& fname : assistant_files) {
                // Extract the title from the filename (strip .json)
                if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".json") {
                    std::string title = fname.substr(0, fname.size() - 5);
                    // Add tab with first provider; load_tab_from_disk will
                    // restore the actual provider from the saved data
                    add_tab(0, title);
                }
            }
        } else {
            // No saved tabs — start with one default tab using first provider
            add_tab(0);
        }
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
            add_tab(0); // always use first provider for new tabs
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

                // Copy title before any mutations/erase (avoids dangling ref UB)
                std::string closed_title = tab.title;

                // Save consolidated assistant data before closing
                save_tab_to_disk(tab, *app_session);

                // Remove from manifest
                app_session->remove_assistant_file(closed_title + ".json");

                // Delete the consolidated JSON file — do this before erase
                // while `tab` is still alive (dangling ref after erase is UB).
                {
                    std::error_code ec;
                    std::filesystem::remove(
                        app_session->session_file_path(closed_title + ".json"), ec);
                }

                free_lotr_name(closed_title);
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
                    render_wiki_tab(wiki, mono_font);
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

                        // 40% session tabs (left) + 60% Chat (right) — no menu bar wrapper
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
                            auto sepPosA =
                                ImVec2(winPos.x + tl.x + planWidth + (gap / 2), winPos.y + tl.y);
                            auto sepPosB = ImVec2(sepPosA.x, winPos.y + tl.y + space.y);

                            // Left panel: session tabs
                            SetCursorPos(planPos);
                            BeginChild("##left-session-tabs",
                                planSize,
                                ImGuiChildFlags_None,
                                ImGuiWindowFlags_None);

                            if (BeginTabBar("##session-tabs")) {
                                // Config first so it's the default focus — model list loads
                                // as soon as the assistant tab is created.
                                if (BeginTabItem("Config")) {
                                    render_config_tab(tab, cfg, mono_font);
                                    EndTabItem();
                                }
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
                                if (BeginTabItem("Notes")) {
                                    render_notes_tab(tab.session->notes(), mono_font);
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

    // ── Save all tabs to consolidated JSON files ──
    for (auto& tab : tabs) {
        save_tab_to_disk(tab, *app_session);
    }

    // ── Update manifest with current tabs ──
    {
        // Rebuild assistant file list from tabs and persist once
        std::vector<std::string> filenames;
        filenames.reserve(tabs.size());
        for (const auto& tab : tabs) {
            filenames.push_back(tab.title + ".json");
        }
        app_session->set_assistant_files(filenames);

        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec) {
            app_session->set_last_cwd(cwd.string());
        }
        // set_assistant_files already called save_manifest()
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
