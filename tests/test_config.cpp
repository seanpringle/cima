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

static fs::path write_dotenv(const std::string& content) {
    auto tmp = fs::temp_directory_path() / "llmchat_test_env_XXXXXX";
    auto fd = mkstemp(tmp.string().data());
    REQUIRE(fd != -1);
    close(fd);
    std::ofstream ofs(tmp);
    ofs << content;
    ofs.close();
    return tmp;
}

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

TEST_CASE("Config defaults", "[config]") {
    unset_env("LLM_API");
    unset_env("LLM_KEY");
    unset_env("MODEL");
    unset_env("SYSTEM_PROMPT");
    unset_env("SAFE_DIR");

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
// .env file loading
// ---------------------------------------------------------------------------

TEST_CASE("Config .env loading", "[config][dotenv]") {
    unset_env("LLM_API");
    unset_env("LLM_KEY");
    unset_env("MODEL");
    unset_env("SYSTEM_PROMPT");
    unset_env("SAFE_DIR");

    auto dotenv = write_dotenv(R"(
# comment line
API_BASE=https://opencode.ai/zen/go/v1
API_KEY=sk-mykey
MODEL=deepseek-v4-flash
SYSTEM_PROMPT=You are a cat.
SAFE_DIR=/tmp/sandbox
)");

    Config::load_dotenv(dotenv);
    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "https://opencode.ai/zen/go/v1");
    REQUIRE(cfg.api_key == "sk-mykey");
    REQUIRE(cfg.model == "deepseek-v4-flash");
    REQUIRE(cfg.system_prompt == "You are a cat.");
    REQUIRE(cfg.safe_dir == fs::weakly_canonical("/tmp/sandbox"));

    fs::remove(dotenv);
}

TEST_CASE("Config .env with export keyword", "[config][dotenv]") {
    unset_env("LLM_API");

    auto dotenv = write_dotenv("export API_BASE=http://exported:9999/v1\n");
    Config::load_dotenv(dotenv);

    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "http://exported:9999/v1");

    fs::remove(dotenv);
}

TEST_CASE("Config .env with quoted value", "[config][dotenv]") {
    unset_env("LLM_API");

    auto dotenv = write_dotenv("API_BASE=\"http://quoted:7777/v1\"\n");
    Config::load_dotenv(dotenv);

    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "http://quoted:7777/v1");

    fs::remove(dotenv);
}

TEST_CASE("Config .env env var takes priority over .env", "[config][dotenv]") {
    set_env("LLM_API", "http://explicit:5555/v1");

    auto dotenv = write_dotenv("API_BASE=http://dotenv:3333/v1\n");
    Config::load_dotenv(dotenv);

    auto cfg = Config::from_env();
    REQUIRE(cfg.api_base == "http://explicit:5555/v1");

    fs::remove(dotenv);
}

TEST_CASE("Config .env missing file is silent", "[config][dotenv]") {
    auto missing = fs::temp_directory_path() / "nonexistent.env";
    REQUIRE(!fs::exists(missing));
    Config::load_dotenv(missing);  // should not crash
}

// ---------------------------------------------------------------------------
// SAFE_DIR canonicalization
// ---------------------------------------------------------------------------

TEST_CASE("Config SAFE_DIR resolves to canonical", "[config]") {
    set_env("SAFE_DIR", "/usr/local/../local");
    auto cfg = Config::from_env();
    REQUIRE(cfg.safe_dir == fs::weakly_canonical("/usr/local"));
}

// ---------------------------------------------------------------------------
// from_env_with_dotenv auto-detect
// ---------------------------------------------------------------------------

TEST_CASE("Config from_env_with_dotenv finds .env in cwd", "[config][dotenv]") {
    unset_env("LLM_API");

    auto cwd = fs::current_path();
    auto dotenv_path = cwd / ".env";
    std::ofstream ofs(dotenv_path);
    ofs << "API_BASE=http://cwd-test:1234/v1\n";
    ofs.close();

    const char* fake_argv[] = {"/some/other/llm-chat", nullptr};
    auto cfg = Config::from_env_with_dotenv(1, const_cast<char**>(fake_argv));
    REQUIRE(cfg.api_base == "http://cwd-test:1234/v1");

    fs::remove(dotenv_path);
}
