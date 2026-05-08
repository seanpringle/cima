#include "config.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

std::atomic<bool> g_interrupted{false};

// ---------------------------------------------------------------------------
// .env file loading
// ---------------------------------------------------------------------------

static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

void Config::load_dotenv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return;  // silently skip missing files
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = trim(line);

        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }

        // strip optional "export " prefix
        std::string_view sv(trimmed);
        constexpr std::string_view export_prefix = "export ";
        if (sv.starts_with(export_prefix)) {
            sv.remove_prefix(export_prefix.size());
        }

        auto eq = sv.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            std::cerr << "warning: skipping malformed .env line: " << trimmed
                      << std::endl;
            continue;
        }

        std::string key(sv.substr(0, eq));
        std::string value(sv.substr(eq + 1));
        key = trim(key);
        value = unquote(value);

        if (!key.empty()) {
            setenv(key.c_str(), value.c_str(), 0);  // 0 = don't override
        }
    }
}

// ---------------------------------------------------------------------------
// Environment variable loading
// ---------------------------------------------------------------------------

Config Config::from_env() {
    Config cfg;

    auto get_env = [](const char* name, const std::string& fallback) {
        const char* val = std::getenv(name);
        return val ? std::string(val) : fallback;
    };

    cfg.api_base = get_env("LLM_API", get_env("API_BASE", cfg.api_base));
    cfg.api_key = get_env("LLM_KEY", get_env("API_KEY", cfg.api_key));
    cfg.model = get_env("MODEL", cfg.model);
    cfg.system_prompt = get_env("SYSTEM_PROMPT", cfg.system_prompt);

    {
        const char* safe = std::getenv("SAFE_DIR");
        if (safe && safe[0]) {
            cfg.safe_dir = safe;
        }
    }

    // resolve safe_dir to canonical form
    if (cfg.safe_dir.empty()) {
        cfg.safe_dir = std::filesystem::current_path().string();
    }
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(
        std::filesystem::path(cfg.safe_dir), ec);
    if (!ec) {
        cfg.safe_dir = canonical.string();
    }

    return cfg;
}

// ---------------------------------------------------------------------------
// Auto-detect .env
// ---------------------------------------------------------------------------

Config Config::from_env_with_dotenv(int argc, char* argv[]) {
    // Try 1: directory of the executable
    if (argc > 0 && argv[0] && argv[0][0]) {
        std::filesystem::path exe = argv[0];
        if (exe.is_relative()) {
            exe = std::filesystem::current_path() / exe;
        }
        auto exe_dir = exe.parent_path();
        auto dotenv = exe_dir / ".env";
        std::error_code ec;
        if (std::filesystem::exists(dotenv, ec)) {
            load_dotenv(dotenv);
            return from_env();
        }
    }

    // Try 2: current working directory
    {
        auto dotenv = std::filesystem::current_path() / ".env";
        std::error_code ec;
        if (std::filesystem::exists(dotenv, ec)) {
            load_dotenv(dotenv);
            return from_env();
        }
    }

    // Try 3: $HOME/.llm-chat.env
    {
        const char* home = std::getenv("HOME");
        if (home) {
            auto dotenv = std::filesystem::path(home) / ".llm-chat.env";
            std::error_code ec;
            if (std::filesystem::exists(dotenv, ec)) {
                load_dotenv(dotenv);
                return from_env();
            }
        }
    }

    return from_env();
}
