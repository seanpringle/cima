#include "gui_app.h"
#include "client.h"
#include "session.h"
#include "session_data.h"
#include "gui_chat.h"
#include "agent.h"
#include "plan.h"

#include <thread>
#include <future>

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

using namespace ImGui;

// ── Global provider model cache ────────────────────────────────────────
ProviderModelCache g_provider_models;

#include <fontconfig/fontconfig.h>

#include <csignal>

// ═══════════════════════════════════════════════════════════════════
// Font lookup helpers
// ═══════════════════════════════════════════════════════════════════

static std::string find_system_font(const std::string& pattern_str) {
    FcPattern* pattern = FcNameParse(reinterpret_cast<const FcChar8*>(pattern_str.c_str()));
    if (!pattern)
        return {};

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

    if (!cfg.font_sans.empty())
        out_sans = cfg.font_sans;
    if (!cfg.font_mono.empty())
        out_mono = cfg.font_mono;

    auto try_family = [&](const std::string& sans_name, const std::string& mono_name) -> bool {
        if (out_sans.empty()) {
            std::string p = find_system_font(sans_name);
            if (p.empty())
                return false;
            out_sans = std::move(p);
        }
        if (out_mono.empty()) {
            std::string p = find_system_font(mono_name);
            if (p.empty())
                return false;
            out_mono = std::move(p);
        }
        return true;
    };

    if (try_family("DejaVu Sans", "DejaVu Sans Mono"))
        return true;
    if (try_family("Liberation Sans", "Liberation Mono"))
        return true;
    if (try_family("sans-serif", "monospace"))
        return true;

    return false;
}

// ═══════════════════════════════════════════════════════════════════
// GuiBootstrap — RAII for SDL + ImGui lifecycle
// ═══════════════════════════════════════════════════════════════════

ImFont* mono_font = nullptr;

void load_fonts(SDL_Window* window) {
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale <= 0.0f)
        display_scale = 1.0f;
    float scale = display_scale;
    ImGui::GetStyle().ScaleAllSizes(scale);

    ImFontConfig font_cfg;
    font_cfg.OversampleH = 2;
    float fs = static_cast<float>(cfg.font_size) * scale;

    std::string sans_path, mono_path;
    if (find_font_pair(sans_path, mono_path)) {
        ImGui::GetIO().Fonts->Clear();
        ImGui::GetIO().Fonts->AddFontFromFileTTF(sans_path.c_str(), fs, &font_cfg);
        mono_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(mono_path.c_str(), fs, &font_cfg);
    }

    if (!mono_font) {
        SDL_Log("Warning: fontconfig unavailable or no fonts found; "
                "using ImGui built-in default font");
        mono_font = ImGui::GetIO().Fonts->Fonts[0];
    }
}

struct GuiBootstrap {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    int result_code = 0;

    GuiBootstrap(const std::string& session_name) {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            SDL_Log("SDL_Init error: %s", SDL_GetError());
            result_code = 1;
            return;
        }

        std::string window_title = "cima :: " + session_name;
        window = SDL_CreateWindow(window_title.c_str(), 1280, 720, SDL_WINDOW_RESIZABLE);
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

        load_fonts(window);

        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
    }

    ~GuiBootstrap() {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        if (renderer)
            SDL_DestroyRenderer(renderer);
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
    }

    GuiBootstrap(const GuiBootstrap&) = delete;
    GuiBootstrap& operator=(const GuiBootstrap&) = delete;
};

// ═══════════════════════════════════════════════════════════════════
// SDL event handling
// ═══════════════════════════════════════════════════════════════════

/// Poll SDL events. Returns true if the application should quit.
static bool handle_sdl_events() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT)
            return true;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            return true;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════
// Frame rendering
// ═══════════════════════════════════════════════════════════════════

static void render_frame(PrimaryAgent& primary, bool& done) {
    SetNextWindowPos(ImVec2(0, 0));
    SetNextWindowSize(GetIO().DisplaySize);
    Begin("cima",
        nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

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
        BeginChild("##left-session-tabs", planSize, ImGuiChildFlags_None, ImGuiWindowFlags_None);

        if (BeginTabBar("##session-tabs")) {
            if (BeginTabItem("   Plan   ")) {
                auto plan_result = primary.session->plan().read_plan();
                if (plan_result) {
                    render_content(*plan_result);
                } else {
                    TextDisabled("(empty plan)");
                }
                EndTabItem();
            }

            if (BeginTabItem("   Config   ")) {
                render_config_tab(primary);
                EndTabItem();
            }

            for (auto& sa_tab : primary.subagents) {
                PushID(sa_tab.id);
                if (BeginTabItem(
                        ("   " + sa_tab.title + "   ##sa-" + std::to_string(sa_tab.id)).c_str())) {
                    render_subagent_tab(sa_tab);
                    render_subagent_chat(sa_tab);
                    EndTabItem();
                }
                PopID();
            }

            EndTabBar();
        }
        EndChild();

        GetWindowDrawList()->AddLine(
            sepPosA, sepPosB, GetColorU32(ImGuiCol_Separator), GetStyle().ChildBorderSize);

        // Right panel: Chat UI
        PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        SetCursorPos(chatPos);
        BeginChild("##tab_chat", chatSize, ImGuiChildFlags_None, ImGuiWindowFlags_None);
        PopStyleVar();
        render_chat_ui(primary, done);
        EndChild();
    }

    End();
}

// ═══════════════════════════════════════════════════════════════════
// gui_main — application entry point (orchestrator)
// ═══════════════════════════════════════════════════════════════════

int gui_main(const std::string& session_name) {
    GuiBootstrap gfx(session_name);
    if (gfx.result_code)
        return gfx.result_code;

    Session session(session_name);
    session.print_welcome();

    PrimaryAgent primary(session.session_data());

    // ── Kick off async model fetch for every provider ──
    for (const auto& provider : cfg.providers) {
        auto& entry = g_provider_models[provider.name];
        std::string api_base = provider.api_base;
        std::string api_key = provider.api_key;
        std::packaged_task<void()> task([&entry, api_base, api_key]() {
            ChatClient client(api_base, api_key);
            auto result = client.fetch_models();
            if (result) {
                entry.models = std::move(*result);
            } else {
                entry.error = std::move(result.error());
            }
            entry.fetched = true;
        });
        std::thread(std::move(task)).detach();
    }

    // Main loop
    bool done = false;
    while (!done) {
        if (handle_sdl_events()) {
            primary.cancel_running_chats();
            done = true;
            break;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        render_frame(primary, done);

        ImGui::Render();
        SDL_SetRenderDrawColor(gfx.renderer, 0, 0, 0, 255);
        SDL_RenderClear(gfx.renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), gfx.renderer);
        SDL_RenderPresent(gfx.renderer);
    }

    return 0;
}
