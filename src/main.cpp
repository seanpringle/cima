#include "config.h"
#include "gui_app.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      std::cout << "Usage: llm-chat [--model <name>] [--api-base <url>]\n\n"
                   "Environment variables:\n"
                   "  LLM_API / API_BASE   API endpoint (default: "
                   "http://127.0.0.1:11000/v1)\n"
                   "  LLM_KEY / API_KEY   API key (optional)\n"
                   "  MODEL               Model name\n"
                   "  SYSTEM_PROMPT       System prompt\n"
                   "  SAFE_DIR            Tool sandbox directory\n"
                << std::flush;
      return 0;
    }
    if (arg == "--model" && i + 1 < argc) {
      setenv("MODEL", argv[++i], 1);
    } else if (arg == "--api-base" && i + 1 < argc) {
      setenv("LLM_API", argv[++i], 1);
    }
  }

  try {
    auto cfg = Config::from_env();
    return gui_main(std::move(cfg));
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << std::endl;
    return 1;
  }
}
