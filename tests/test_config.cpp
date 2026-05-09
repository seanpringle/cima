#include "config.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void unset_env(const char* name) { unsetenv(name); }

static void set_env(const char* name, const char* value) {
    setenv(name, value, 1);
}

static void clear_all_config_env() {
    unset_env("LLM_API");
    unset_env("LLM_KEY");
    unset_env("API_BASE");
    unset_env("API_KEY");
    unset_env("MODEL");
    unset_env("SYSTEM_PROMPT");
    unset_env("SAFE_DIR");
    unset_env("LLM_MAX_TOOL_ITERATIONS");
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_CASE("Config defaults", "[config]") {
    clear_all_config_env();

    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "http://127.0.0.1:11000/v1");
    REQUIRE(cfg.api_key == "");
    REQUIRE(cfg.model == "deepseek-v4-flash");
    REQUIRE(cfg.system_prompt == "You are a helpful assistant.");
    REQUIRE(cfg.safe_dir == fs::current_path());
}

// ---------------------------------------------------------------------------
// Environment variable overrides
// ---------------------------------------------------------------------------

TEST_CASE("Config env vars", "[config]") {
    clear_all_config_env();
    set_env("LLM_API", "http://test:8080/v1");
    set_env("LLM_KEY", "sk-test123");
    set_env("MODEL", "gpt-4");
    set_env("SYSTEM_PROMPT", "Be brief.");
    set_env("SAFE_DIR", "/tmp");

    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "http://test:8080/v1");
    REQUIRE(cfg.api_key == "sk-test123");
    REQUIRE(cfg.model == "gpt-4");
    REQUIRE(cfg.system_prompt == "Be brief.");
    REQUIRE(cfg.safe_dir == fs::weakly_canonical("/tmp"));
}

// ---------------------------------------------------------------------------
// SAFE_DIR canonicalization
// ---------------------------------------------------------------------------

TEST_CASE("Config SAFE_DIR resolves to canonical", "[config]") {
    clear_all_config_env();
    set_env("SAFE_DIR", "/usr/local/../local");
    auto cfg = Config::from_env();
    REQUIRE(cfg.safe_dir == fs::weakly_canonical("/usr/local"));
}
