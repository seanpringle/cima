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
    cfg.planner_prompt = get_env("LLM_PLANNER_PROMPT",
        get_env("PLANNER_PROMPT", cfg.planner_prompt));
    cfg.builder_prompt = get_env("LLM_BUILDER_PROMPT",
        get_env("BUILDER_PROMPT", cfg.builder_prompt));

    {
        const char* val = std::getenv("LLM_MAX_TOOL_ITERATIONS");
        if (val && val[0]) {
            int n = std::atoi(val);
            if (n > 0)
                cfg.max_tool_iterations = n;
        }
    }

    {
        const char* val = std::getenv("LLM_CONTEXT_LIMIT");
        if (val && val[0]) {
            int n = std::atoi(val);
            if (n > 0)
                cfg.context_limit = n;
        }
    }

    {
        const char* val = std::getenv("LLM_COMPACT_THRESHOLD");
        if (val && val[0]) {
            int n = std::atoi(val);
            if (n > 0 && n <= 100)
                cfg.compact_threshold = n;
        }
    }

    cfg.search_api_key = get_env("SEARCH_API_KEY", "");
    cfg.search_engine_id = get_env("SEARCH_ENGINE_ID", "");
    cfg.search_endpoint = get_env("SEARCH_ENDPOINT", "");

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

    // ---- read-only paths whitelist ----
    // Default paths always included
    cfg.read_only_paths = {"/usr/include", "/usr/share/doc"};

    // READ_ONLY_PATHS env var (colon-separated) adds extra paths
    {
        const char* rop = std::getenv("READ_ONLY_PATHS");
        if (rop && rop[0]) {
            std::string s(rop);
            size_t start = 0;
            while (start < s.size()) {
                auto colon = s.find(':', start);
                std::string p = (colon == std::string::npos)
                                    ? s.substr(start)
                                    : s.substr(start, colon - start);
                // Trim leading whitespace
                while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
                    p.erase(0, 1);
                // Trim trailing whitespace
                while (!p.empty() && (p.back() == ' ' || p.back() == '\t'))
                    p.pop_back();
                if (!p.empty()) {
                    std::error_code ec2;
                    auto canonical2 =
                        std::filesystem::weakly_canonical(std::filesystem::path(p), ec2);
                    if (!ec2) {
                        cfg.read_only_paths.push_back(canonical2.string());
                    } else {
                        cfg.read_only_paths.push_back(p);
                    }
                }
                start = (colon == std::string::npos) ? s.size() : colon + 1;
            }
        }
    }

    return cfg;
}
