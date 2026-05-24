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
    REQUIRE(cfg.SYSTEM_PROMPT.find("AI coding assistant") != std::string::npos);
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
    p.reasoning_efforts = {"low", "high"};
    p.context_limit = 64000;
    cfg.providers.push_back(p);

    cfg.read_only_paths = {"/custom/path"};

    auto j = cfg.to_json();
    // SYSTEM_PROMPT must NOT be in JSON
    REQUIRE_FALSE(j.contains("SYSTEM_PROMPT"));

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
            lp.reasoning_effort = pj.value("reasoning_effort", std::string());
            lp.reasoning_efforts = pj.value("reasoning_efforts", std::vector<std::string>());
            if (lp.reasoning_efforts.empty()) {
                lp.reasoning_efforts = {"low", "medium", "high"};
            }
            lp.context_limit = pj.value("context_limit", 300000);
            loaded.providers.push_back(std::move(lp));
        }
    }

    if (j.contains("read_only_paths") && j["read_only_paths"].is_array()) {
        loaded.read_only_paths.clear();
        for (const auto& p : j["read_only_paths"])
            loaded.read_only_paths.push_back(p.get<std::string>());
    }

    REQUIRE(loaded.providers.size() == 1);
    REQUIRE(loaded.providers[0].name == "test-provider");
    REQUIRE(loaded.providers[0].api_base == "http://test:8080/v1");
    REQUIRE(loaded.providers[0].api_key == "sk-test123");
    REQUIRE(loaded.providers[0].model == "gpt-4");
    REQUIRE(loaded.providers[0].reasoning_effort == "low");
    REQUIRE(loaded.providers[0].reasoning_efforts.size() == 2);
    REQUIRE(loaded.providers[0].reasoning_efforts[0] == "low");
    REQUIRE(loaded.providers[0].reasoning_efforts[1] == "high");
    REQUIRE(loaded.providers[0].context_limit == 64000);
    // read_only_paths was replaced; load() will add defaults back, but our
    // manual overlay didn't — just check the JSON value was read
    REQUIRE(loaded.read_only_paths.size() == 1);
    REQUIRE(loaded.read_only_paths[0] == "/custom/path");
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

// ---------------------------------------------------------------------------
// MCP endpoint tests
// ---------------------------------------------------------------------------

TEST_CASE("McpEndpoint defaults", "[config][mcp]") {
    // Default-constructed McpEndpoint should have sensible defaults.
    McpEndpoint m;
    REQUIRE(m.name.empty());
    REQUIRE(m.transport == "stdio");
    REQUIRE(m.command.empty());
    REQUIRE(m.args.empty());
    REQUIRE(m.cwd.empty());
    REQUIRE(m.url.empty());
    REQUIRE(m.api_key.empty());
    REQUIRE(m.description.empty());
    REQUIRE(m.env.empty());
    REQUIRE(m.timeout_sec == 60);
}

TEST_CASE("McpEndpoint round-trip serialization", "[config][mcp]") {
    Config cfg;

    // Stdio endpoint
    McpEndpoint stdio_mcp;
    stdio_mcp.name = "my-filesystem";
    stdio_mcp.transport = "stdio";
    stdio_mcp.command = "npx";
    stdio_mcp.args = {"-y", "@modelcontextprotocol/server-filesystem", "/tmp/test"};
    stdio_mcp.cwd = "/home/user";
    stdio_mcp.description = "File system access for the project directory";
    stdio_mcp.env = {{"NODE_ENV", "production"}};
    stdio_mcp.timeout_sec = 120;
    cfg.mcp_servers.push_back(stdio_mcp);

    // Streamable HTTP endpoint
    McpEndpoint http_mcp;
    http_mcp.name = "remote-api";
    http_mcp.transport = "streamable-http";
    http_mcp.url = "http://localhost:3100/mcp";
    http_mcp.api_key = "sk-test-key";
    http_mcp.timeout_sec = 30;
    cfg.mcp_servers.push_back(http_mcp);

    auto j = cfg.to_json();
    REQUIRE(j.contains("mcp_servers"));
    REQUIRE(j["mcp_servers"].is_array());
    REQUIRE(j["mcp_servers"].size() == 2);

    // Now simulate deserialization (like load() does)
    Config loaded;
    if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
        for (const auto& mj : j["mcp_servers"]) {
            McpEndpoint m;
            m.name = mj.value("name", std::string());
            m.transport = mj.value("transport", std::string("stdio"));
            m.command = mj.value("command", std::string());
            m.url = mj.value("url", std::string());
            m.api_key = mj.value("api_key", std::string());
            m.cwd = mj.value("cwd", std::string());
            m.timeout_sec = mj.value("timeout_sec", 60);
            m.description = mj.value("description", std::string());

            if (mj.contains("args") && mj["args"].is_array()) {
                for (const auto& a : mj["args"]) {
                    if (a.is_string()) m.args.push_back(a.get<std::string>());
                }
            }
            if (mj.contains("env") && mj["env"].is_object()) {
                for (auto it = mj["env"].begin(); it != mj["env"].end(); ++it) {
                    if (it.value().is_string()) m.env[it.key()] = it.value().get<std::string>();
                }
            }
            loaded.mcp_servers.push_back(std::move(m));
        }
    }

    REQUIRE(loaded.mcp_servers.size() == 2);

    // Check stdio endpoint
    CHECK(loaded.mcp_servers[0].name == "my-filesystem");
    CHECK(loaded.mcp_servers[0].transport == "stdio");
    CHECK(loaded.mcp_servers[0].command == "npx");
    CHECK(loaded.mcp_servers[0].args.size() == 3);
    CHECK(loaded.mcp_servers[0].args[0] == "-y");
    CHECK(loaded.mcp_servers[0].args[1] == "@modelcontextprotocol/server-filesystem");
    CHECK(loaded.mcp_servers[0].args[2] == "/tmp/test");
    CHECK(loaded.mcp_servers[0].cwd == "/home/user");
    CHECK(loaded.mcp_servers[0].description == "File system access for the project directory");
    CHECK(loaded.mcp_servers[0].env.size() == 1);
    CHECK(loaded.mcp_servers[0].env.at("NODE_ENV") == "production");
    CHECK(loaded.mcp_servers[0].timeout_sec == 120);

    // Check HTTP endpoint
    CHECK(loaded.mcp_servers[1].name == "remote-api");
    CHECK(loaded.mcp_servers[1].transport == "streamable-http");
    CHECK(loaded.mcp_servers[1].url == "http://localhost:3100/mcp");
    CHECK(loaded.mcp_servers[1].api_key == "sk-test-key");
    CHECK(loaded.mcp_servers[1].timeout_sec == 30);
    // HTTP endpoint has no command/args
    CHECK(loaded.mcp_servers[1].command.empty());
    CHECK(loaded.mcp_servers[1].args.empty());
}

TEST_CASE("McpEndpoint missing optional fields load as empty", "[config][mcp]") {
    // JSON without cwd, api_key, env should load with empty defaults.
    json j = R"({
        "mcp_servers": [
            {
                "name": "minimal",
                "command": "my-server"
            }
        ]
    })"_json;

    Config loaded;
    if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
        for (const auto& mj : j["mcp_servers"]) {
            McpEndpoint m;
            m.name = mj.value("name", std::string());
            m.transport = mj.value("transport", std::string("stdio"));
            m.command = mj.value("command", std::string());
            m.url = mj.value("url", std::string());
            m.api_key = mj.value("api_key", std::string());
            m.cwd = mj.value("cwd", std::string());
            m.timeout_sec = mj.value("timeout_sec", 60);

            if (mj.contains("args") && mj["args"].is_array()) {
                for (const auto& a : mj["args"]) {
                    if (a.is_string()) m.args.push_back(a.get<std::string>());
                }
            }
            if (mj.contains("env") && mj["env"].is_object()) {
                for (auto it = mj["env"].begin(); it != mj["env"].end(); ++it) {
                    if (it.value().is_string()) m.env[it.key()] = it.value().get<std::string>();
                }
            }
            loaded.mcp_servers.push_back(std::move(m));
        }
    }

    REQUIRE(loaded.mcp_servers.size() == 1);
    auto& m = loaded.mcp_servers[0];
    CHECK(m.name == "minimal");
    CHECK(m.transport == "stdio");      // default
    CHECK(m.command == "my-server");
    CHECK(m.cwd.empty());               // missing → empty
    CHECK(m.url.empty());               // missing → empty
    CHECK(m.api_key.empty());           // missing → empty
    CHECK(m.description.empty());       // missing → empty
    CHECK(m.env.empty());               // missing → empty
    CHECK(m.args.empty());              // missing → empty
    CHECK(m.timeout_sec == 60);         // default
}

TEST_CASE("Config MCP + provider coexistence", "[config][mcp]") {
    // Existing provider parsing still works when mcp_servers array is present.
    json j = R"({
        "providers": [
            {
                "name": "test-provider",
                "api_base": "http://localhost:11000/v1",
                "api_key": "sk-test",
                "model": "test-model"
            }
        ],
        "mcp_servers": [
            {
                "name": "my-server",
                "command": "my-server-bin"
            }
        ]
    })"_json;

    // Parse providers
    Config loaded;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            Provider p;
            p.name = pj.value("name", std::string());
            p.api_base = pj.value("api_base", std::string());
            p.api_key = pj.value("api_key", std::string());
            p.model = pj.value("model", std::string());
            loaded.providers.push_back(std::move(p));
        }
    }

    // Parse MCP servers
    if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
        for (const auto& mj : j["mcp_servers"]) {
            McpEndpoint m;
            m.name = mj.value("name", std::string());
            m.command = mj.value("command", std::string());
            loaded.mcp_servers.push_back(std::move(m));
        }
    }

    REQUIRE(loaded.providers.size() == 1);
    CHECK(loaded.providers[0].name == "test-provider");
    CHECK(loaded.providers[0].api_base == "http://localhost:11000/v1");

    REQUIRE(loaded.mcp_servers.size() == 1);
    CHECK(loaded.mcp_servers[0].name == "my-server");
    CHECK(loaded.mcp_servers[0].command == "my-server-bin");
}

TEST_CASE("Config MCP servers absent does not break providers", "[config][mcp]") {
    // Without mcp_servers array, provider parsing still works fine.
    json j = R"({
        "providers": [
            {
                "name": "only-provider",
                "api_base": "http://localhost:11000/v1",
                "api_key": "sk-test",
                "model": "test"
            }
        ]
    })"_json;

    Config loaded;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            Provider p;
            p.name = pj.value("name", std::string());
            p.api_base = pj.value("api_base", std::string());
            p.api_key = pj.value("api_key", std::string());
            p.model = pj.value("model", std::string());
            loaded.providers.push_back(std::move(p));
        }
    }

    REQUIRE(loaded.providers.size() == 1);
    CHECK(loaded.providers[0].name == "only-provider");
    // mcp_servers should be empty by default
    CHECK(loaded.mcp_servers.empty());
}

// ---------------------------------------------------------------------------
// Reasoning efforts tests
// ---------------------------------------------------------------------------

TEST_CASE("reasoning_effort defaults to empty when absent", "[config]") {
    json j = R"({
        "providers": [
            {
                "name": "test",
                "api_base": "http://test/v1",
                "api_key": "sk-test",
                "model": "gpt-4"
            }
        ]
    })"_json;

    Provider p;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            p.name = pj.value("name", std::string());
            p.api_base = pj.value("api_base", std::string());
            p.api_key = pj.value("api_key", std::string());
            p.model = pj.value("model", std::string());
            p.reasoning_effort = pj.value("reasoning_effort", std::string());
            p.reasoning_efforts = pj.value("reasoning_efforts", std::vector<std::string>());
            if (p.reasoning_efforts.empty()) {
                p.reasoning_efforts = {"low", "medium", "high"};
            }
        }
    }

    CHECK(p.reasoning_effort.empty());
    CHECK(p.reasoning_efforts.size() == 3);
    CHECK(p.reasoning_efforts[0] == "low");
    CHECK(p.reasoning_efforts[1] == "medium");
    CHECK(p.reasoning_efforts[2] == "high");
}

TEST_CASE("reasoning_efforts empty array defaults to low/medium/high", "[config]") {
    json j = R"({
        "providers": [
            {
                "name": "test",
                "api_base": "http://test/v1",
                "api_key": "sk-test",
                "model": "gpt-4",
                "reasoning_efforts": []
            }
        ]
    })"_json;

    Provider p;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            p.name = pj.value("name", std::string());
            p.api_base = pj.value("api_base", std::string());
            p.api_key = pj.value("api_key", std::string());
            p.model = pj.value("model", std::string());
            p.reasoning_effort = pj.value("reasoning_effort", std::string());
            p.reasoning_efforts = pj.value("reasoning_efforts", std::vector<std::string>());
            if (p.reasoning_efforts.empty()) {
                p.reasoning_efforts = {"low", "medium", "high"};
            }
        }
    }

    CHECK(p.reasoning_efforts.size() == 3);
    CHECK(p.reasoning_efforts[0] == "low");
    CHECK(p.reasoning_efforts[1] == "medium");
    CHECK(p.reasoning_efforts[2] == "high");
}

TEST_CASE("reasoning_efforts custom values preserved", "[config]") {
    json j = R"({
        "providers": [
            {
                "name": "test",
                "api_base": "http://test/v1",
                "api_key": "sk-test",
                "model": "gpt-4",
                "reasoning_effort": "conservative",
                "reasoning_efforts": ["conservative", "balanced", "creative"]
            }
        ]
    })"_json;

    Provider p;
    if (j.contains("providers") && j["providers"].is_array()) {
        for (const auto& pj : j["providers"]) {
            p.name = pj.value("name", std::string());
            p.api_base = pj.value("api_base", std::string());
            p.api_key = pj.value("api_key", std::string());
            p.model = pj.value("model", std::string());
            p.reasoning_effort = pj.value("reasoning_effort", std::string());
            p.reasoning_efforts = pj.value("reasoning_efforts", std::vector<std::string>());
            if (p.reasoning_efforts.empty()) {
                p.reasoning_efforts = {"low", "medium", "high"};
            }
        }
    }

    CHECK(p.reasoning_effort == "conservative");
    REQUIRE(p.reasoning_efforts.size() == 3);
    CHECK(p.reasoning_efforts[0] == "conservative");
    CHECK(p.reasoning_efforts[1] == "balanced");
    CHECK(p.reasoning_efforts[2] == "creative");
}

TEST_CASE("McpEndpoint equality operator", "[config][mcp]") {
    McpEndpoint a;
    a.name = "test";
    a.command = "server";
    a.timeout_sec = 60;

    McpEndpoint b;
    b.name = "test";
    b.command = "server";
    b.timeout_sec = 60;

    CHECK(a == b);

    b.timeout_sec = 120;
    CHECK_FALSE(a == b);
}

// ---------------------------------------------------------------------------
// Subagent config tests
// ---------------------------------------------------------------------------

TEST_CASE("SubagentConfig defaults", "[config][subagent]") {
    SubagentConfig sa;
    CHECK(sa.name.empty());
    CHECK(sa.description.empty());
    CHECK(sa.read_only == false);
}

TEST_CASE("SubagentConfig round-trip serialization", "[config][subagent]") {
    Config cfg;

    SubagentConfig sa1;
    sa1.name = "debugger";
    sa1.description = "Debug issues in the codebase";
    sa1.read_only = true;
    cfg.subagents.push_back(sa1);

    SubagentConfig sa2;
    sa2.name = "writer";
    sa2.description = "Write documentation";
    sa2.read_only = false;
    cfg.subagents.push_back(sa2);

    auto j = cfg.to_json();
    REQUIRE(j.contains("subagents"));
    REQUIRE(j["subagents"].is_array());
    REQUIRE(j["subagents"].size() == 2);

    // Simulate deserialization
    std::vector<SubagentConfig> loaded;
    if (j.contains("subagents") && j["subagents"].is_array()) {
        for (const auto& saj : j["subagents"]) {
            SubagentConfig sa;
            sa.name = saj.value("name", std::string());
            sa.description = saj.value("description", std::string());
            sa.read_only = saj.value("read_only", false);
            if (!sa.name.empty()) {
                loaded.push_back(std::move(sa));
            }
        }
    }

    REQUIRE(loaded.size() == 2);
    CHECK(loaded[0].name == "debugger");
    CHECK(loaded[0].description == "Debug issues in the codebase");
    CHECK(loaded[0].read_only == true);
    CHECK(loaded[1].name == "writer");
    CHECK(loaded[1].description == "Write documentation");
    CHECK(loaded[1].read_only == false);
}

TEST_CASE("SubagentConfig missing optional fields load as defaults", "[config][subagent]") {
    json j = R"({
        "subagents": [
            {
                "name": "minimal"
            }
        ]
    })"_json;

    std::vector<SubagentConfig> loaded;
    if (j.contains("subagents") && j["subagents"].is_array()) {
        for (const auto& saj : j["subagents"]) {
            SubagentConfig sa;
            sa.name = saj.value("name", std::string());
            sa.description = saj.value("description", std::string());
            sa.read_only = saj.value("read_only", false);
            if (!sa.name.empty()) {
                loaded.push_back(std::move(sa));
            }
        }
    }

    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "minimal");
    CHECK(loaded[0].description.empty());
    CHECK(loaded[0].read_only == false);
}

TEST_CASE("SubagentConfig empty name skipped", "[config][subagent]") {
    json j = R"({
        "subagents": [
            {
                "name": "",
                "description": "should be skipped"
            },
            {
                "name": "valid",
                "description": "kept"
            }
        ]
    })"_json;

    std::vector<SubagentConfig> loaded;
    if (j.contains("subagents") && j["subagents"].is_array()) {
        for (const auto& saj : j["subagents"]) {
            SubagentConfig sa;
            sa.name = saj.value("name", std::string());
            sa.description = saj.value("description", std::string());
            sa.read_only = saj.value("read_only", false);
            if (!sa.name.empty()) {
                loaded.push_back(std::move(sa));
            }
        }
    }

    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "valid");
}

TEST_CASE("SubagentConfig read_only true from JSON", "[config][subagent]") {
    json j = R"({
        "subagents": [
            {
                "name": "read-only-agent",
                "read_only": true
            }
        ]
    })"_json;

    std::vector<SubagentConfig> loaded;
    if (j.contains("subagents") && j["subagents"].is_array()) {
        for (const auto& saj : j["subagents"]) {
            SubagentConfig sa;
            sa.name = saj.value("name", std::string());
            sa.description = saj.value("description", std::string());
            sa.read_only = saj.value("read_only", false);
            if (!sa.name.empty()) {
                loaded.push_back(std::move(sa));
            }
        }
    }

    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].name == "read-only-agent");
    CHECK(loaded[0].read_only == true);
}

TEST_CASE("knob defaults are defined as constexpr constants", "[config][knobs]") {
    CHECK(kDefaultMaxToolIterations == 100);
    CHECK(kDefaultSubagentTimeout == 600);
    CHECK(kDefaultBashTimeout == 30);
    CHECK(kDefaultGrepTimeout == 10);
    CHECK(kDefaultWebSearchTimeout == 15);
    CHECK(kDefaultWebFetchTimeout == 15);
}
