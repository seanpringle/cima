#include "config.h"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

TEST_CASE("Config defaults", "[config]") {
    // Default-constructed Config should have built-in defaults.
    Config cfg;
    // providers is empty by default (load() populates it)
    REQUIRE(cfg.providers.empty());
    REQUIRE(cfg.system_prompt.find("AI coding assistant") != std::string::npos);
    REQUIRE(cfg.max_tool_iterations == 100);
    // read_only_paths is empty until load() adds defaults
    REQUIRE(cfg.read_only_paths.empty());
}

TEST_CASE("Config to_json / round-trip", "[config]") {
    Config cfg;

    // Set up a provider
    Provider p;
    p.name = "test-provider";
    p.api_base = "http://test:8080/v1";
    p.api_key = "sk-test123";
    p.model = "gpt-4";
    p.reasoning_effort = "low";
    p.context_limit = 64000;
    cfg.providers.push_back(p);

    cfg.search_api_key = "search-key";
    cfg.search_engine_id = "engine-id";
    cfg.search_endpoint = "https://custom.search";
    cfg.read_only_paths = {"/custom/path"};
    cfg.max_tool_iterations = 50;

    auto j = cfg.to_json();
    // system_prompt must NOT be in JSON
    REQUIRE_FALSE(j.contains("system_prompt"));

    // Now simulate what load() does: parse JSON and overlay on a fresh Config
    Config loaded;

    // Parse providers
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            Provider lp;
            lp.name = pj.value("name", std::string());
            lp.api_base = pj.value("api_base", std::string());
            lp.api_key = pj.value("api_key", std::string());
            lp.model = pj.value("model", std::string());
            lp.reasoning_effort = pj.value("reasoning_effort", std::string("high"));
            lp.context_limit = pj.value("context_limit", 300000);
            loaded.providers.push_back(std::move(lp));
        }
    }

    if (j.contains("search_api_key"))   loaded.search_api_key = j["search_api_key"].get<std::string>();
    if (j.contains("search_engine_id")) loaded.search_engine_id = j["search_engine_id"].get<std::string>();
    if (j.contains("search_endpoint"))  loaded.search_endpoint = j["search_endpoint"].get<std::string>();
    if (j.contains("read_only_paths") && j["read_only_paths"].is_array()) {
        loaded.read_only_paths.clear();
        for (const auto& p : j["read_only_paths"])
            loaded.read_only_paths.push_back(p.get<std::string>());
    }
    if (j.contains("max_tool_iterations") && j["max_tool_iterations"].is_number_integer())
        loaded.max_tool_iterations = j["max_tool_iterations"].get<int>();

    REQUIRE(loaded.providers.size() == 1);
    REQUIRE(loaded.providers[0].name == "test-provider");
    REQUIRE(loaded.providers[0].api_base == "http://test:8080/v1");
    REQUIRE(loaded.providers[0].api_key == "sk-test123");
    REQUIRE(loaded.providers[0].model == "gpt-4");
    REQUIRE(loaded.providers[0].reasoning_effort == "low");
    REQUIRE(loaded.providers[0].context_limit == 64000);
    REQUIRE(loaded.search_api_key == "search-key");
    REQUIRE(loaded.search_engine_id == "engine-id");
    REQUIRE(loaded.search_endpoint == "https://custom.search");
    // read_only_paths was replaced; load() will add defaults back, but our
    // manual overlay didn't — just check the JSON value was read
    REQUIRE(loaded.read_only_paths.size() == 1);
    REQUIRE(loaded.read_only_paths[0] == "/custom/path");
    REQUIRE(loaded.max_tool_iterations == 50);
}

TEST_CASE("Config read_only_paths defaults added if missing", "[config]") {
    // Simulate load()'s default-insertion logic.
    Config cfg;
    cfg.read_only_paths.clear();

    bool has_usr_include = false, has_usr_doc = false;
    for (const auto& p : cfg.read_only_paths) {
        if (p == "/usr/include")   has_usr_include = true;
        if (p == "/usr/share/doc") has_usr_doc = true;
    }
    if (!has_usr_include) cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/include");
    if (!has_usr_doc)     cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/share/doc");

    REQUIRE(cfg.read_only_paths.size() == 2);
    // /usr/share/doc was inserted second at begin(), so it's first
    REQUIRE(cfg.read_only_paths[0] == "/usr/share/doc");
    REQUIRE(cfg.read_only_paths[1] == "/usr/include");
}

TEST_CASE("Config read_only_paths preserves user entries", "[config]") {
    Config cfg;
    cfg.read_only_paths = {"/custom/path"};

    bool has_usr_include = false, has_usr_doc = false;
    for (const auto& p : cfg.read_only_paths) {
        if (p == "/usr/include")   has_usr_include = true;
        if (p == "/usr/share/doc") has_usr_doc = true;
    }
    if (!has_usr_include) cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/include");
    if (!has_usr_doc)     cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/share/doc");

    REQUIRE(cfg.read_only_paths.size() == 3);
    REQUIRE(cfg.read_only_paths[0] == "/usr/share/doc");
    REQUIRE(cfg.read_only_paths[1] == "/usr/include");
    REQUIRE(cfg.read_only_paths[2] == "/custom/path");
}
