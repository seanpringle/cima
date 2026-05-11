#include "config.h"
#include "gui_app.h"

#include <curl/curl.h>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Must be called once before any other libcurl function.
    curl_global_init(CURL_GLOBAL_ALL);
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout
                << "Usage: llm-chat [--model <name>] [--api-base <url>]\n\n"
                << "Environment variables:\n"
                << "  LLM_API / API_BASE   API endpoint (default: http://127.0.0.1:11000/v1)\n"
                << "  LLM_KEY / API_KEY   API key (optional)\n"
                << "  MODEL               Model name\n"
                << "  LLM_SYSTEM_PROMPT / SYSTEM_PROMPT  System prompt for chat sessions (default: built-in prompt)\n"
                << "  SAFE_DIR            Tool sandbox directory\n"
                << "  SEARCH_API_KEY      Google Custom Search API key (for web_search)\n"
                << "  SEARCH_ENGINE_ID    Google Custom Search engine ID\n"
                << "  SEARCH_ENDPOINT     Custom search endpoint with {query} placeholder\n"
                << "                     (default: Wikipedia opensearch, no key needed)\n"
                << "  READ_ONLY_PATHS     Colon-separated extra paths for read-only tools\n"
                << "                     (default: /usr/include:/usr/share/doc)\n"
                << std::flush;
            return 0;
        }
        if (arg == "--model" && i + 1 < argc) {
            setenv("MODEL", argv[++i], 1);
        } else if (arg == "--api-base" && i + 1 < argc) {
            setenv("LLM_API", argv[++i], 1);
        }
    }

    int exit_code = 0;
    try {
        auto cfg = Config::from_env();
        exit_code = gui_main(std::move(cfg));
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        exit_code = 1;
    }
    curl_global_cleanup();
    return exit_code;
}
