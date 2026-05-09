#include "config.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <unistd.h>

std::atomic<bool> g_interrupted{false};

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
        const char* val = std::getenv("LLM_MAX_TOOL_ITERATIONS");
        if (val && val[0]) {
            int n = std::atoi(val);
            if (n > 0)
                cfg.max_tool_iterations = n;
        }
    }

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
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path(cfg.safe_dir), ec);
    if (!ec) {
        cfg.safe_dir = canonical.string();
    }

    return cfg;
}
