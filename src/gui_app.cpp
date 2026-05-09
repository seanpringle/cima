#include "gui_app.h"
#include "gui_chat.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"

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

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = NULL;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();

  ImFont* mono_font = nullptr;
  {
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale <= 0.0f) display_scale = 1.0f;

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
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansMath-Regular.ttf", fs, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansSymbols-Regular.ttf", fs, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansSymbols2-Regular.ttf", fs, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoEmoji-Regular.ttf", fs, &merge, unicode);

    mono_font = ImGui::GetIO().Fonts->AddFontFromFileTTF("font/DejaVuSansMono.ttf", fs * 0.9f, &cfg, latin);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/DejaVuSansMono.ttf", fs * 0.9f, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansMath-Regular.ttf", fs * 0.9f, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansSymbols-Regular.ttf", fs * 0.9f, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoSansSymbols2-Regular.ttf", fs * 0.9f, &merge, unicode);
    ImGui::GetIO().Fonts->AddFontFromFileTTF("font/NotoEmoji-Regular.ttf", fs * 0.9f, &merge, unicode);
  }

  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);

  ChatSession session(std::move(cfg));
  ChatUIState ui_state;
  ui_state.mono_font = mono_font;
  AsyncChatState chat_state;
  strncpy(ui_state.model_buf, session.model().c_str(), sizeof(ui_state.model_buf) - 1);

  session.set_output_callback([&chat_state](const std::string& text, OutputType type) {
    std::lock_guard<std::mutex> lock(chat_state.mutex);
    chat_state.pending.emplace_back(text, type);
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

    if (done && chat_state.running) {
      g_interrupted = true;
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    render_chat_ui(ui_state, chat_state, session, done);

    ImGui::Render();
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
    SDL_RenderPresent(renderer);
  }

  if (chat_state.future.valid()) {
    chat_state.future.wait();
    try {
      chat_state.future.get();
    } catch (const std::exception& e) {
      std::cerr << "chat error on shutdown: " << e.what() << std::endl;
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
