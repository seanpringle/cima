#include "config.h"
#include "gui_app.h"

#include <curl/curl.h>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <signal.h>
#include <string>

static void print_usage() {
    std::cout
        << "Usage: cima [--force] [<session>]\n\n"
        << "Arguments:\n"
        << "  <session>           Session name (default: \"default\")\n"
        << "                      Stored in ~/.local/state/cima/<session>/\n\n"
        << "Options:\n"
        << "  --force             Skip session integrity checks (warn + continue)\n"
        << "  -h, --help          Print this help\n\n"
        << "Configuration file: ~/.config/cima/cima.json\n"
        << "  Created automatically on first run with default values.\n"
        << "  Edit it to persist settings across sessions.\n"
        << std::flush;
}

int main(int argc, char* argv[]) {
    // Ignore SIGPIPE — writing to a broken pipe (e.g. MCP server exited)
    // should return EPIPE, not kill the process with signal 13.
    signal(SIGPIPE, SIG_IGN);

    // Must be called once before any other libcurl function.
    curl_global_init(CURL_GLOBAL_ALL);

    // Seed RNG for DDG rate-limit jitter
    std::srand(static_cast<unsigned>(std::time(nullptr)));

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
        if (arg == "--force") {
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
        session_name = "default";
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
