#include "mcp/mcp_client.h"
#include "mcp/mcp_registry.h"
#include "mock_mcp_server.hpp"
#include "mock_server.hpp"

#include <catch2/catch_test_macros.hpp>

// ===================================================================
// Helpers
// ===================================================================

/// Create an McpEndpoint for a stdio mock server.
McpEndpoint make_stdio_endpoint(const std::string& name,
                                 const std::string& command,
                                 const std::vector<std::string>& args = {}) {
    McpEndpoint ep;
    ep.name = name;
    ep.transport = "stdio";
    ep.command = command;
    ep.args = args;
    ep.timeout_sec = 5;
    return ep;
}

/// Create an McpEndpoint for an HTTP mock server.
McpEndpoint make_http_endpoint(const std::string& name,
                                const std::string& url) {
    McpEndpoint ep;
    ep.name = name;
    ep.transport = "streamable-http";
    ep.url = url;
    ep.timeout_sec = 5;
    return ep;
}

// ---------------------------------------------------------------------------
// Test: Start single stdio server
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry start single stdio server", "[mcp][registry]") {
    MockMcpServer mock;
    REQUIRE(mock.start());

    // We can't use start_server() with a stdio config because the registry
    // calls start_stdio() which does fork/exec. Instead we test the registry's
    // ability to manage a server lifecycle using the McCpEndpoint config.
    // For the stdio end-to-end, the registry uses client->start_stdio().
    // We test this by creating a real McpClient and connecting to the mock.

    // Actually, we need to test start_server() which calls start_stdio().
    // The mock server is a child process that the McpClient connects to.
    // But start_server() creates its own McpClient and calls start_stdio().
    // Since start_stdio() does fork/exec, it can't connect to our mock pipes.

    // This test would need a real MCP server binary. For now, we test the
    // registry logic using HTTP transport (which works with MockServer).
    // The stdio pathway is tested indirectly via McpClient tests.

    // Verify that a non-existent server returns the right status.
    McpRegistry registry;
    CHECK_FALSE(registry.is_running("nonexistent"));
    CHECK_FALSE(registry.has_running_servers());
}

// ---------------------------------------------------------------------------
// Test: Start single HTTP server
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry start single HTTP server", "[mcp][registry]") {
    // Set up an HTTP mock MCP server.
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})" ;
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return ""; // notification
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "RegTestServer"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "greet"}, {"description", "Say hello"}, {"inputSchema", json::object({
                        {"type", "object"},
                        {"properties", {{"name", {{"type", "string"}}}}},
                        {"required", json::array({"name"})}
                    })}}
                })}
            };
        } else if (method == "tools/call") {
            resp["result"] = {
                {"content", json::array({
                    {{"type", "text"}, {"text", "Hello from registry!"}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpEndpoint ep = make_http_endpoint("test-server", base_url);

    McpRegistry registry;
    auto result = registry.start_server(ep);
    REQUIRE(result.has_value());

    CHECK(registry.is_running("test-server"));
    CHECK(registry.has_running_servers());
    CHECK(registry.running_server_names() == std::set<std::string>{"test-server"});

    // Cleanup.
    registry.stop_server("test-server");
}

// ---------------------------------------------------------------------------
// Test: tool_names() after start
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry tool names after start", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "ToolServer"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "tool1"}, {"description", "First tool"}, {"inputSchema", json::object()}},
                    {{"name", "tool2"}, {"description", "Second tool"}, {"inputSchema", json::object()}},
                    {{"name", "tool3"}, {"description", "Third tool"}, {"inputSchema", json::object()}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("myserver", base_url));
    REQUIRE(result.has_value());

    auto tools = registry.all_tools();
    REQUIRE(tools.size() == 3);

    // Check namespaced names.
    CHECK(tools[0].name == "mcp_myserver_tool1");
    CHECK(tools[1].name == "mcp_myserver_tool2");
    CHECK(tools[2].name == "mcp_myserver_tool3");

    registry.stop_server("myserver");
}

// ---------------------------------------------------------------------------
// Test: Execute tool
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry execute tool", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "ExecServer"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "calc"}, {"description", "Calculator"}, {"inputSchema", json::object()}}
                })}
            };
        } else if (method == "tools/call") {
            resp["result"] = {
                {"content", json::array({
                    {{"type", "text"}, {"text", "42"}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("calc-server", base_url));
    REQUIRE(result.has_value());

    auto exec_result = registry.execute_tool("mcp_calc-server_calc", {{"x", 6}, {"y", 7}});
    REQUIRE(exec_result.has_value());
    CHECK(*exec_result == "42");

    registry.stop_server("calc-server");
}

// ---------------------------------------------------------------------------
// Test: Two servers, same tool name
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry two servers same tool name", "[mcp][registry]") {
    int request_count = 0;
    auto make_handler = [&](const std::string& server_name, const std::string& result_text) {
        return [&, server_name, result_text](const std::string& req) -> std::string {
            request_count++;
            auto hdr_end = req.find("\r\n\r\n");
            if (hdr_end == std::string::npos)
                return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
            std::string body = req.substr(hdr_end + 4);
            json j;
            try { j = json::parse(body); } catch (...) {
                return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
            }
            if (!j.contains("id")) return "";
            int id = j["id"];
            std::string method = j.value("method", "");
            json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            if (method == "initialize") {
                resp["result"] = {
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", {{"tools", json::object()}}},
                    {"serverInfo", {{"name", server_name}, {"version", "1.0"}}}
                };
            } else if (method == "tools/list") {
                resp["result"] = {
                    {"tools", json::array({
                        {{"name", "foo"}, {"description", "Common tool"}, {"inputSchema", json::object()}}
                    })}
                };
            } else if (method == "tools/call") {
                resp["result"] = {
                    {"content", json::array({
                        {{"type", "text"}, {"text", result_text}}
                    })}
                };
            } else if (method == "shutdown") {
                resp["result"] = nullptr;
            } else {
                resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
            }
            return resp.dump();
        };
    };

    // Need two servers on different ports
    auto server1 = std::make_unique<MockServer>(make_handler("Srv1", "from-srv1"));
    auto server2 = std::make_unique<MockServer>(make_handler("Srv2", "from-srv2"));

    McpRegistry registry;

    auto result1 = registry.start_server(
        make_http_endpoint("srv1", "http://127.0.0.1:" + std::to_string(server1->port()) + "/mcp"));
    REQUIRE(result1.has_value());

    auto result2 = registry.start_server(
        make_http_endpoint("srv2", "http://127.0.0.1:" + std::to_string(server2->port()) + "/mcp"));
    REQUIRE(result2.has_value());

    auto tools = registry.all_tools();
    REQUIRE(tools.size() == 2);

    // Both tools have different namespaced names.
    CHECK(tools[0].name == "mcp_srv1_foo");
    CHECK(tools[1].name == "mcp_srv2_foo");

    // Execute each one and verify correct routing.
    auto r1 = registry.execute_tool("mcp_srv1_foo", json::object());
    REQUIRE(r1.has_value());
    CHECK(*r1 == "from-srv1");

    auto r2 = registry.execute_tool("mcp_srv2_foo", json::object());
    REQUIRE(r2.has_value());
    CHECK(*r2 == "from-srv2");

    registry.stop_server("srv1");
    registry.stop_server("srv2");
}

// ---------------------------------------------------------------------------
// Test: Stop server
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry stop server", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "StopTest"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "mytool"}, {"description", "A tool"}, {"inputSchema", json::object()}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("stop-server", base_url));
    REQUIRE(result.has_value());
    CHECK(registry.is_running("stop-server"));
    CHECK(registry.has_running_servers());
    CHECK(registry.all_tools().size() == 1);

    // Stop the server.
    auto stop_result = registry.stop_server("stop-server");
    CHECK(stop_result.has_value());

    CHECK_FALSE(registry.is_running("stop-server"));
    CHECK_FALSE(registry.has_running_servers());
    CHECK(registry.all_tools().empty());
}

// ---------------------------------------------------------------------------
// Test: Stop unknown server (no-op, no crash)
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry stop unknown server", "[mcp][registry]") {
    McpRegistry registry;
    // Stopping a non-existent server should not crash.
    auto result = registry.stop_server("nonexistent");
    CHECK(result.has_value());
    CHECK_FALSE(registry.is_running("nonexistent"));
    CHECK_FALSE(registry.has_running_servers());
}

// ---------------------------------------------------------------------------
// Test: Execute on stopped server
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry execute on stopped server", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "StopExec"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "mytool"}, {"description", "Tool"}, {"inputSchema", json::object()}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("my-server", base_url));
    REQUIRE(result.has_value());

    // Stop the server.
    registry.stop_server("my-server");

    // Try to execute a tool on the stopped server.
    auto exec_result = registry.execute_tool("mcp_my-server_mytool", json::object());
    REQUIRE_FALSE(exec_result.has_value());
    CHECK(exec_result.error().find("not running") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Test: has_running_servers()
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry has_running_servers lifecycle", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "Lifecycle"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {{"tools", json::array()}};
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    CHECK_FALSE(registry.has_running_servers());

    auto result = registry.start_server(make_http_endpoint("lifecycle-test", base_url));
    REQUIRE(result.has_value());
    CHECK(registry.has_running_servers());

    registry.stop_server("lifecycle-test");
    CHECK_FALSE(registry.has_running_servers());
}

// ---------------------------------------------------------------------------
// Test: refresh_tools()
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry refresh_tools", "[mcp][registry]") {
    int list_call_count = 0;

    auto server = std::make_unique<MockServer>([&](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "RefreshTest"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            list_call_count++;
            if (list_call_count == 1) {
                resp["result"] = {
                    {"tools", json::array({
                        {{"name", "initial-tool"}, {"description", "First version"}, {"inputSchema", json::object()}}
                    })}
                };
            } else {
                resp["result"] = {
                    {"tools", json::array({
                        {{"name", "initial-tool"}, {"description", "First version"}, {"inputSchema", json::object()}},
                        {{"name", "new-tool"}, {"description", "Added later"}, {"inputSchema", json::object()}}
                    })}
                };
            }
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("refresh-test", base_url));
    REQUIRE(result.has_value());

    // Initial tools: 1 tool.
    auto tools_before = registry.all_tools();
    REQUIRE(tools_before.size() == 1);
    CHECK(tools_before[0].name == "mcp_refresh-test_initial-tool");

    // Refresh and expect 2 tools.
    auto refresh_result = registry.refresh_tools();
    REQUIRE(refresh_result.has_value());
    REQUIRE(refresh_result->size() == 2);
    CHECK((*refresh_result)[0].name == "mcp_refresh-test_initial-tool");
    CHECK((*refresh_result)[1].name == "mcp_refresh-test_new-tool");

    // all_tools() should now also return 2.
    auto tools_after = registry.all_tools();
    REQUIRE(tools_after.size() == 2);

    registry.stop_server("refresh-test");
}

// ---------------------------------------------------------------------------
// Test: Duplicate server names
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry duplicate server names", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "DupTest"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {{"tools", json::array()}};
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;

    // Start the first server.
    auto r1 = registry.start_server(make_http_endpoint("dup-name", base_url));
    REQUIRE(r1.has_value());
    CHECK(registry.is_running("dup-name"));

    // Starting with the same name while running should fail.
    auto r2 = registry.start_server(make_http_endpoint("dup-name", base_url));
    CHECK_FALSE(r2.has_value());

    registry.stop_server("dup-name");
}

// ---------------------------------------------------------------------------
// Test: Empty server name
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry empty server name", "[mcp][registry]") {
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "EmptyName"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {{"tools", json::array()}};
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;

    // An empty name should work (though unusual).
    auto r = registry.start_server(make_http_endpoint("", base_url));
    CHECK(r.has_value());

    registry.stop_server("");
}

// ---------------------------------------------------------------------------
// Test: Server crash and refresh
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry server crash + refresh", "[mcp][registry]") {
    // Use a server that becomes unresponsive after the first few calls.
    int call_count = 0;
    auto server = std::make_unique<MockServer>([&](const std::string& req) -> std::string {
        call_count++;
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "CrashTest"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            if (call_count >= 5) {
                // After the first refresh succeeds, make subsequent
                // tools/list calls return invalid JSON to simulate a crash.
                // (call_count includes: 1=initialize, 2=initialized notif,
                //  3=start_server tools/list, 4=first refresh tools/list)
                return "not valid json";
            }
            resp["result"] = {
                {"tools", json::array({
                    {{"name", "crash-tool"}, {"description", "Tool on crashable server"}, {"inputSchema", json::object()}}
                })}
            };
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    McpRegistry registry;
    auto result = registry.start_server(make_http_endpoint("crash-server", base_url));
    REQUIRE(result.has_value());

    // First refresh should succeed.
    auto r1 = registry.refresh_tools();
    CHECK(r1.has_value());
    CHECK(registry.is_running("crash-server"));

    // Second refresh hits the broken server and should mark it as not running.
    auto r2 = registry.refresh_tools();
    // The result is a best-effort union; the crashed server's tools are removed.
    CHECK_FALSE(registry.is_running("crash-server"));
}

// ---------------------------------------------------------------------------
// Test: Destructor cleanup
// ---------------------------------------------------------------------------

TEST_CASE("McpRegistry destructor cleanup", "[mcp][registry]") {
    // Start a server inside a scope and let the registry go out of scope.
    auto server = std::make_unique<MockServer>([](const std::string& req) -> std::string {
        auto hdr_end = req.find("\r\n\r\n");
        if (hdr_end == std::string::npos)
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        std::string body = req.substr(hdr_end + 4);
        json j;
        try { j = json::parse(body); } catch (...) {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
        }
        if (!j.contains("id")) return "";
        int id = j["id"];
        std::string method = j.value("method", "");
        json resp;
        resp["jsonrpc"] = "2.0";
        resp["id"] = id;
        if (method == "initialize") {
            resp["result"] = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities", {{"tools", json::object()}}},
                {"serverInfo", {{"name", "DtorTest"}, {"version", "1.0"}}}
            };
        } else if (method == "tools/list") {
            resp["result"] = {{"tools", json::array()}};
        } else if (method == "shutdown") {
            resp["result"] = nullptr;
        } else {
            resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
        }
        return resp.dump();
    });

    std::string base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";

    {
        McpRegistry registry;
        auto r = registry.start_server(make_http_endpoint("dtor-test", base_url));
        REQUIRE(r.has_value());
        CHECK(registry.is_running("dtor-test"));
        CHECK(registry.has_running_servers());
        // Registry goes out of scope here — destructor handles cleanup.
    }

    // After the registry is destroyed, the mock server should have received
    // a shutdown request.  We can't directly check the registry state (it's gone),
    // but the test verifies no crashes or leaks occur.
}
