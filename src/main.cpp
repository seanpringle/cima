#include "config.h"
#include "gui_app.h"

#include <curl/curl.h>
#include <iostream>
#include <string>

static void print_usage() {
    std::cout
        << "Usage: cima [--model <name>] [--api-base <url>] [--force] <session>\n\n"
        << "Arguments:\n"
        << "  <session>           Session name (resume or create a new one)\n"
        << "                      Stored in ~/.local/state/cima/<session>/\n\n"
        << "Options:\n"
        << "  --model <name>      Model name\n"
        << "  --api-base <url>    API endpoint (default: http://127.0.0.1:11000/v1)\n"
        << "  --force             Skip session integrity checks (warn + continue)\n"
        << "  -h, --help          Print this help\n\n"
        << "Configuration file: ~/.config/cima/cima.json\n"
        << "  Created automatically on first run with default values.\n"
        << "  Edit it to persist settings across sessions.\n"
        << "  CLI flags override file values.\n"
        << std::flush;
}

int main(int argc, char* argv[]) {
    // Must be called once before any other libcurl function.
    curl_global_init(CURL_GLOBAL_ALL);

    std::string session_name;
    bool force = false;

    // Load config from file first
    auto cfg = Config::load();

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--model" && i + 1 < argc) {
            cfg.model = argv[++i];
        } else if (arg == "--api-base" && i + 1 < argc) {
            cfg.api_base = argv[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (arg[0] == '-') {
            std::cerr << "Unknown option: " << arg << "\n\n";
            print_usage();
            return 1;
        } else if (session_name.empty()) {
            // First non-option argument is the session name
            session_name = arg;
        } else {
            std::cerr << "Unexpected argument: " << arg << "\n\n";
            print_usage();
            return 1;
        }
    }

    if (session_name.empty()) {
        std::cerr << "Error: <session> argument is required.\n\n";
        print_usage();
        return 1;
    }

    int exit_code = 0;
    try {
        exit_code = gui_main(std::move(cfg), session_name, force);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        exit_code = 1;
    }
    curl_global_cleanup();
    return exit_code;
}
