#include "chat.h"
#include "config.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef USE_READLINE
#include <readline/history.h>
#include <readline/readline.h>
#endif

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_interrupted{false};

extern "C" void handle_sigint(int /*sig*/) {
    g_interrupted = true;
    _exit(130);
}

// ---------------------------------------------------------------------------
// Readline wrapper / plain cin fallback
// ---------------------------------------------------------------------------

static std::string history_file() {
    const char* home = std::getenv("HOME");
    return home ? (std::string(home) + "/.llm-chat-history")
               : ".llm-chat-history";
}

#ifdef USE_READLINE

static std::string read_input() {
    char* line = readline("> ");
    if (!line)
        return "/exit";
    std::string result(line);
    free(line);
    if (!result.empty())
        add_history(result.c_str());
    return result;
}

static char* command_generator(const char* text, int state) {
    static int idx;
    static const char* cmds[] = {"/clear", "/exit", "/help",
                                 "/model", "/quit", nullptr};
    if (state == 0)
        idx = 0;
    while (cmds[idx]) {
        const char* c = cmds[idx++];
        if (std::strncmp(c, text, std::strlen(text)) == 0)
            return strdup(c);
    }
    return nullptr;
}

static char** completion(const char* text, int, int) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
}

static void init_console() {
    using_history();
    rl_attempted_completion_function = completion;
    auto hf = history_file();
    read_history(hf.c_str());
}

static void save_console() {
    auto hf = history_file();
    write_history(hf.c_str());
}

#else

static std::string read_input() {
    std::string line;
    std::cout << "> " << std::flush;
    if (!std::getline(std::cin, line))
        return "/exit";
    return line;
}

static void init_console() {}
static void save_console() {}

#endif

// ---------------------------------------------------------------------------
// Banner
// ---------------------------------------------------------------------------

static void print_banner(const Config& cfg) {
    std::string auth = cfg.api_key.empty() ? "" : "  [auth: bearer]";
    std::cout << "llm-chat \xE2\x94\x80 " << cfg.api_base
              << "/chat/completions"
              << "  model: " << cfg.model
              << "  tools: list_files, read_file, grep_files, write_file, "
                 "run_bash"
              << auth << std::endl;
    std::cout << "/exit  /clear  /model <name>  /help" << std::endl;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    try {
        std::signal(SIGINT, handle_sigint);

        // CLI overrides
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout
                    << "Usage: llm-chat [--model <name>] [--api-base <url>]\n\n"
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

        auto cfg = Config::from_env_with_dotenv(argc, argv);
        print_banner(cfg);
        ChatSession session(std::move(cfg));

        bool in_reasoning = false;
        session.set_output_callback(
            [&](const std::string& text, OutputType type) {
                if (type == OutputType::Reasoning) {
                    if (!in_reasoning) {
                        std::cout << "\033[90m";
                        in_reasoning = true;
                    }
                    std::cout << text << std::flush;
                } else if (type == OutputType::Content) {
                    if (in_reasoning) {
                        std::cout << "\033[0m\n";
                        in_reasoning = false;
                    }
                    std::cout << text << std::flush;
                } else {
                    std::cerr << text << std::endl;
                }
            });

        init_console();

        bool running = true;
        while (running && !g_interrupted) {
            auto line = read_input();
            if (line.empty()) continue;

            if (line == "/exit" || line == "/quit") {
                running = false;
                break;
            }
            if (line == "/clear") {
                session.clear();
                std::cout << "cleared" << std::endl;
                continue;
            }
            if (line == "/help") {
                std::cout << "Commands:\n"
                             "  /exit           Exit\n"
                             "  /quit           Exit\n"
                             "  /clear          Clear conversation\n"
                             "  /model <name>   Switch model\n"
                             "  /help           This help\n";
                continue;
            }
            if (line.rfind("/model ", 0) == 0) {
                auto name = line.substr(7);
                if (!name.empty()) {
                    session.set_model(name);
                    std::cout << "model: " << session.model() << std::endl;
                } else {
                    std::cout << "usage: /model <name>" << std::endl;
                }
                continue;
            }
            if (line[0] == '/') {
                std::cout << "unknown command: " << line << std::endl;
                continue;
            }

            g_interrupted = false;
            in_reasoning = false;

            auto result = session.run_once(line);

            if (in_reasoning) std::cout << "\033[0m";
            if (g_interrupted) {
                std::cout << "\ninterrupted" << std::endl;
                continue;
            }
            if (!result) {
                std::cerr << "error: " << result.error() << std::endl;
                continue;
            }
            std::cout << std::endl;
        }

        save_console();
        std::cout << "bye!" << std::endl;
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << std::endl;
        return 1;
    }
}
