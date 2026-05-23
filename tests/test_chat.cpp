#include "chat.h"
#include "mock_server.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>

// ===================================================================
// Helpers
// ===================================================================

// Parse JSON body out of an HTTP request string
static json parse_request_body(const std::string& http_request) {
    auto hdr_end = http_request.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        return nullptr;
    }
    auto body = http_request.substr(hdr_end + 4);
    return json::parse(body, nullptr, false);
}

// ===================================================================
// Helpers
// ===================================================================

static std::string make_tool_call_sse(const std::string& name,
                                      const std::string& args,
                                      const std::string& id = "call_abc") {
    json delta = {{"choices",
                   {{{"index", 0},
                     {"delta",
                      {{"role", "assistant"},
                       {"content", nullptr},
                       {"tool_calls",
                        {{{"index", 0},
                          {"id", id},
                          {"type", "function"},
                          {"function",
                           {{"name", name}, {"arguments", args}}}}}}}}}}}};
    return "data: " + delta.dump() + "\n\ndata: [DONE]\n\n";
}

static std::string make_content_sse(const std::string& content) {
    json delta = {{"choices",
                   {{{"index", 0},
                     {"delta", {{"role", "assistant"}, {"content", content}}}}}}};
    return "data: " + delta.dump() + "\n\ndata: [DONE]\n\n";
}

static std::string make_reasoning_sse(const std::string& reasoning,
                                      const std::string& content) {
    json delta = {{"choices",
                   {{{"index", 0},
                     {"delta",
                      {{"role", "assistant"},
                       {"reasoning_content", reasoning},
                       {"content", content}}}}}}};
    return "data: " + delta.dump() + "\n\ndata: [DONE]\n\n";
}

// Build a test Config + Provider pair
struct TestConfig {
    Config cfg;
    Provider provider;
};

static TestConfig make_test_config(const std::string& base_url,
    const std::string& model = "test-model",
    const std::string& reasoning_effort = "high",
    int context_limit = 1000) {
    TestConfig tc;
    tc.provider.name = "test";
    tc.provider.api_base = base_url;
    tc.provider.api_key = "";
    tc.provider.model = model;
    tc.provider.reasoning_effort = reasoning_effort;
    tc.provider.context_limit = context_limit;
    return tc;
}

/// Create an McpEndpoint for an HTTP mock server.
static McpEndpoint make_http_endpoint(const std::string& name,
                                       const std::string& url) {
    McpEndpoint ep;
    ep.name = name;
    ep.transport = "streamable-http";
    ep.url = url;
    ep.timeout_sec = 5;
    return ep;
}

// ===================================================================
// Simple Q&A
// ===================================================================

TEST_CASE("ChatSession simple Q&A", "[chat]") {
    MockServer server(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello, World!");
        },
        true);

    auto tc = make_test_config(server.base_url());
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("Say hi");
    REQUIRE(result);
    CHECK(result->content == "Hello, World!");
    CHECK(result->reasoning.empty());
}

// ===================================================================
// Payload includes reasoning_effort
// ===================================================================

TEST_CASE("ChatSession payload includes reasoning_effort", "[chat]") {
    std::string last_request;
    MockServer server(
        [&](const std::string& req) -> std::string {
            last_request = req;
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(server.base_url());
    tc.provider.reasoning_effort = "high";
    tc.provider.context_limit = 1000;  // avoid model discovery request

    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("Say hi");
    REQUIRE(result);

    auto body = parse_request_body(last_request);
    REQUIRE(!body.is_discarded());
    REQUIRE(body.contains("reasoning_effort"));
    CHECK(body["reasoning_effort"] == "high");
}

TEST_CASE("ChatSession omits reasoning_effort when empty", "[chat]") {
    std::string last_request;
    MockServer server(
        [&](const std::string& req) -> std::string {
            last_request = req;
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(server.base_url());
    tc.provider.reasoning_effort = "";   // empty — should omit
    tc.provider.context_limit = 1000;    // avoid model discovery request

    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("Say hi");
    REQUIRE(result);

    auto body = parse_request_body(last_request);
    REQUIRE(!body.is_discarded());
    REQUIRE_FALSE(body.contains("reasoning_effort"));
}

// ===================================================================
// Tool call triggered
// ===================================================================

TEST_CASE("ChatSession tool call then content", "[chat]") {
    int call_count = 0;
    MockServer server(
        [&](const std::string& req) -> std::string {
            call_count++;
            if (call_count == 1) {
                // First request: return a tool call
                return make_tool_call_sse("read_file",
                                          R"({"path": "test.txt", "start_line": 1, "end_line": 1})");
            }
            // Second request: return content
            return make_content_sse("I found some files.");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test");
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("List files");
    REQUIRE(result);
    CHECK(result->content == "I found some files.");
    CHECK(call_count == 2);
}

// ===================================================================
// 10-iteration guard
// ===================================================================

TEST_CASE("ChatSession max tool iterations", "[chat]") {
    int call_count = 0;
    MockServer server(
        [&](const std::string&) -> std::string {
            call_count++;
            return make_tool_call_sse("read_file",
                                      R"({"path": "test.txt", "start_line": 1, "end_line": 1})",
                                      "call_" + std::to_string(call_count));
        },
        true);

    auto tc = make_test_config(server.base_url(), "test", "high", 300000);

    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    session.set_max_iterations(10); // override for test
    auto result = session.run_once("List files forever");
    CHECK(result);
    CHECK(result->content.find("Tool call budget exhausted") !=
          std::string::npos);
    // One extra call for model discovery (GET /v1/models), then 10 tool iterations
    CHECK(call_count == 11);
}

// ===================================================================
// Reasoning content
// ===================================================================

TEST_CASE("ChatSession reasoning content preserved", "[chat]") {
    MockServer server(
        [](const std::string&) -> std::string {
            return make_reasoning_sse("I am thinking about this.",
                                     "The answer is 42.");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test");
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("What is 6*7?");
    REQUIRE(result);
    CHECK(result->content == "The answer is 42.");
    CHECK(result->reasoning == "I am thinking about this.");
}

// ===================================================================
// Reasoning with tool calls
// ===================================================================

TEST_CASE("ChatSession reasoning with tool calls", "[chat]") {
    int call_count = 0;
    MockServer server(
        [&](const std::string&) -> std::string {
            call_count++;
            if (call_count == 1) {
                json delta = {
                    {"choices",
                     {{{"index", 0},
                       {"delta",
                        {{"role", "assistant"},
                         {"reasoning_content", "I need to check the files."},
                         {"content", nullptr},
                         {"tool_calls",
                          {{{"index", 0},
                            {"id", "call_xyz"},
                            {"type", "function"},
                            {"function",
                             {{"name", "read_file"},
                              {"arguments",
                               R"({"path": "test.txt", "start_line": 1, "end_line": 1})"}}}}}}}}}}}};
                return "data: " + delta.dump() + "\n\ndata: [DONE]\n\n";
            }
            return make_content_sse("Here is what I found.");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test");
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("Check files");
    REQUIRE(result);
    CHECK(result->content == "Here is what I found.");
    CHECK(call_count == 2);
}

// ===================================================================
// Multi-chunk tool call arguments
// ===================================================================

TEST_CASE("ChatSession multi-chunk tool call args", "[chat]") {
    int call_count = 0;
    MockServer server(
        [&](const std::string&) -> std::string {
            call_count++;
            if (call_count == 1) {
                json c1, c2, c3;
                c1["choices"][0]["delta"]["role"] = "assistant";
                c1["choices"][0]["delta"]["content"] = nullptr;
                c1["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
                c1["choices"][0]["delta"]["tool_calls"][0]["id"] = "call_1";
                c1["choices"][0]["delta"]["tool_calls"][0]["type"] = "function";
                c1["choices"][0]["delta"]["tool_calls"][0]["function"]["name"] = "read_file";
                c1["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = "";

                c2["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
                c2["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = R"({"path": )";

                c3["choices"][0]["delta"]["tool_calls"][0]["index"] = 0;
                c3["choices"][0]["delta"]["tool_calls"][0]["function"]["arguments"] = R"("/tmp/foo.txt"})";

                return "data: " + c1.dump() + "\n\ndata: " + c2.dump() +
                       "\n\ndata: " + c3.dump() + "\n\ndata: [DONE]\n\n";
            }
            return make_content_sse("Read the file.");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test");
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);
    auto result = session.run_once("Read file");
    REQUIRE(result);
    CHECK(result->content == "Read the file.");
    CHECK(call_count == 2);
}

// ===================================================================
// Cancellation token passed to constructor cancels requests
// ===================================================================

TEST_CASE("ChatSession cancelled token aborts request", "[chat]") {
    auto token = make_cancellation_token();
    *token = true;  // pre-cancel

    MockServer server(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test");

    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider, token);
    auto result = session.run_once("Say hi");
    CHECK_FALSE(result);
    // curl should abort the request because the progress callback sees the
    // cancelled token
    CHECK(result.error().find("curl error") != std::string::npos);
}

// ===================================================================
// MCP integration tests
// ===================================================================

/// Helper: create a simple MCP-over-HTTP mock server.
/// Returns a pair (MockServer, base_url). The handler responds to:
///   initialize, tools/list, tools/call, shutdown
struct McpTestServer {
    std::unique_ptr<MockServer> server;
    std::string base_url;
    int tool_count = 1;
    std::string tool_name = "mcp-tool";
    std::string tool_result = "mcp-result";

    void start() {
        server = std::make_unique<MockServer>(
            [this](const std::string& req) -> std::string {
                auto hdr_end = req.find("\r\n\r\n");
                if (hdr_end == std::string::npos)
                    return R"({"jsonrpc":"2.0","id":1,"error":{"code":-32700}})";
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
                        {"serverInfo", {{"name", "McpTestServer"}, {"version", "1.0"}}}
                    };
                } else if (method == "tools/list") {
                    json tools = json::array();
                    for (int i = 0; i < tool_count; i++) {
                        std::string name = tool_name;
                        if (tool_count > 1) name += std::to_string(i + 1);
                        tools.push_back({
                            {"name", name},
                            {"description", "A test tool"},
                            {"inputSchema", {
                                {"type", "object"},
                                {"properties", {{"input", {{"type", "string"}}}}},
                                {"required", json::array()}
                            }}
                        });
                    }
                    resp["result"] = {{"tools", tools}};
                } else if (method == "tools/call") {
                    resp["result"] = {
                        {"content", json::array({
                            {{"type", "text"}, {"text", tool_result}}
                        })}
                    };
                } else if (method == "shutdown") {
                    resp["result"] = nullptr;
                } else {
                    resp["error"] = {{"code", -32601}, {"message", "Unknown"}};
                }
                return resp.dump();
            });
        base_url = "http://127.0.0.1:" + std::to_string(server->port()) + "/mcp";
    }
};

TEST_CASE("ChatSession start_mcp_server registers tools", "[chat][mcp]") {
    McpTestServer mcp;
    mcp.tool_count = 2;
    mcp.tool_name = "greet";
    mcp.start();

    // LLM mock — just returns a simple response so run_once succeeds.
    MockServer llm(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    // Start the MCP server.
    McpEndpoint ep = make_http_endpoint("test-srv", mcp.base_url);
    auto result = session.start_mcp_server(ep);
    REQUIRE(result.has_value());

    // Verify tools are in the ToolRegistry with mcp_ prefix.
    const auto& tools = session.tools_for_testing().tools();
    bool found_tool1 = false, found_tool2 = false;
    for (const auto& t : tools) {
        if (t.name == "mcp_test-srv_greet1") found_tool1 = true;
        if (t.name == "mcp_test-srv_greet2") found_tool2 = true;
    }
    CHECK(found_tool1);
    CHECK(found_tool2);

    // Cleanup.
    session.stop_mcp_server("test-srv");
}

TEST_CASE("ChatSession stop_mcp_server unregisters tools", "[chat][mcp]") {
    McpTestServer mcp;
    mcp.start();

    MockServer llm(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    McpEndpoint ep = make_http_endpoint("srv", mcp.base_url);
    auto result = session.start_mcp_server(ep);
    REQUIRE(result.has_value());

    // Verify tool is registered.
    bool found = false;
    for (const auto& t : session.tools_for_testing().tools()) {
        if (t.name == "mcp_srv_mcp-tool") found = true;
    }
    CHECK(found);

    // Stop the server.
    session.stop_mcp_server("srv");

    // Verify tool is unregistered.
    found = false;
    for (const auto& t : session.tools_for_testing().tools()) {
        if (t.name == "mcp_srv_mcp-tool") found = true;
    }
    CHECK_FALSE(found);
}

TEST_CASE("ChatSession MCP system prompt included when servers running", "[chat][mcp]") {
    std::string last_request;
    McpTestServer mcp;
    mcp.start();

    MockServer llm(
        [&](const std::string& req) -> std::string {
            last_request = req;
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    // Without MCP servers — snippet should be absent.
    auto body = parse_request_body(last_request);
    // Run once without MCP to capture the request.
    // But we need the MCP server started first. Let's check the prompt
    // by making a request after starting.

    McpEndpoint ep = make_http_endpoint("prompt-test", mcp.base_url);
    auto result = session.start_mcp_server(ep);
    REQUIRE(result.has_value());

    // Now run once — the payload should include the MCP snippet.
    last_request.clear();
    auto run_result = session.run_once("Test");
    REQUIRE(run_result.has_value());

    // Parse the request body.
    body = parse_request_body(last_request);
    REQUIRE(!body.is_discarded());
    REQUIRE(body.contains("messages"));
    auto msgs = body["messages"];
    REQUIRE(msgs.is_array());
    REQUIRE(msgs.size() >= 1);

    // The system message should contain the MCP tools text.
    bool found_mcp_snippet = false;
    for (const auto& msg : msgs) {
        if (msg["role"] == "system") {
            std::string content = msg["content"];
            if (content.find("MCP tools") != std::string::npos &&
                content.find("mcp_<servername>_") != std::string::npos) {
                found_mcp_snippet = true;
                break;
            }
        }
    }
    CHECK(found_mcp_snippet);

    session.stop_mcp_server("prompt-test");
}

TEST_CASE("ChatSession MCP system prompt excluded when no servers", "[chat][mcp]") {
    std::string last_request;
    MockServer llm(
        [&](const std::string& req) -> std::string {
            last_request = req;
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    // Run once without any MCP servers.
    auto run_result = session.run_once("Test");
    REQUIRE(run_result.has_value());

    // Parse the request body.
    auto body = parse_request_body(last_request);
    REQUIRE(!body.is_discarded());
    REQUIRE(body.contains("messages"));
    auto msgs = body["messages"];
    REQUIRE(msgs.is_array());

    // The system message should NOT contain MCP tools text.
    bool found_mcp_snippet = false;
    for (const auto& msg : msgs) {
        if (msg["role"] == "system") {
            std::string content = msg["content"];
            if (content.find("MCP tools") != std::string::npos) {
                found_mcp_snippet = true;
                break;
            }
        }
    }
    CHECK_FALSE(found_mcp_snippet);
}

TEST_CASE("ChatSession MCP tool execution through run_once", "[chat][mcp]") {
    McpTestServer mcp;
    mcp.tool_name = "calc";
    mcp.tool_result = "42";
    mcp.start();

    int llm_call_count = 0;
    MockServer llm(
        [&](const std::string& req) -> std::string {
            llm_call_count++;
            if (llm_call_count == 1) {
                // First call: return a tool call for the MCP tool.
                return make_tool_call_sse("mcp_calc-srv_calc", R"({"input": "6*7"})");
            }
            // Second call: return content.
            return make_content_sse("The result is 42.");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    McpEndpoint ep = make_http_endpoint("calc-srv", mcp.base_url);
    auto result = session.start_mcp_server(ep);
    REQUIRE(result.has_value());

    auto run_result = session.run_once("Calculate 6*7");
    REQUIRE(run_result.has_value());
    CHECK(run_result->content == "The result is 42.");
    CHECK(llm_call_count == 2);

    session.stop_mcp_server("calc-srv");
}

TEST_CASE("ChatSession multiple MCP servers", "[chat][mcp]") {
    McpTestServer mcp1;
    mcp1.tool_name = "tool-a";
    mcp1.start();

    McpTestServer mcp2;
    mcp2.tool_name = "tool-b";
    mcp2.start();

    MockServer llm(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    McpEndpoint ep1 = make_http_endpoint("server-a", mcp1.base_url);
    auto r1 = session.start_mcp_server(ep1);
    REQUIRE(r1.has_value());

    McpEndpoint ep2 = make_http_endpoint("server-b", mcp2.base_url);
    auto r2 = session.start_mcp_server(ep2);
    REQUIRE(r2.has_value());

    // Both sets of tools should be registered.
    const auto& tools = session.tools_for_testing().tools();
    bool found_a = false, found_b = false;
    for (const auto& t : tools) {
        if (t.name == "mcp_server-a_tool-a") found_a = true;
        if (t.name == "mcp_server-b_tool-b") found_b = true;
    }
    CHECK(found_a);
    CHECK(found_b);

    session.stop_mcp_server("server-a");
    session.stop_mcp_server("server-b");
}

TEST_CASE("ChatSession MCP server start error", "[chat][mcp]") {
    // An invalid URL should cause start_mcp_server to return an error.
    McpEndpoint ep = make_http_endpoint("bad-server", "http://127.0.0.1:1/mcp");
    ep.timeout_sec = 1; // short timeout for fast test

    MockServer llm(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello!");
        },
        true);

    auto tc = make_test_config(llm.base_url(), "test", "high", 300000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    auto result = session.start_mcp_server(ep);
    CHECK_FALSE(result.has_value());

    // Session should still be usable after the error.
    auto run_result = session.run_once("Test");
    REQUIRE(run_result.has_value());
    CHECK(run_result->content == "Hello!");
}

// ===================================================================
// Tool gate access check — disabled tools are rejected at execution
// ===================================================================

TEST_CASE("ChatSession disabled tool is not executed", "[chat][gates]") {
    int call_count = 0;
    MockServer server(
        [&](const std::string& req) -> std::string {
            call_count++;
            // Request 1: GET /v1/models (model discovery) — return empty JSON object
            if (call_count == 1)
                return "{\"object\":\"list\",\"data\":[]}";
            // Request 2: first chat completion — return a tool call for "read_file"
            if (call_count == 2)
                return make_tool_call_sse("read_file",
                                          R"({"path": "test.txt", "start_line": 1, "end_line": 1})");
            // Subsequent requests: just return content
            return make_content_sse("done");
        },
        true);

    auto tc = make_test_config(server.base_url(), "test", "high", 1000);
    ChatSession session(std::make_shared<Config>(tc.cfg), tc.provider);

    // Disable the tool the LLM is about to call
    session.set_tool_enabled("read_file", false);

    auto result = session.run_once("List files");
    REQUIRE(result);

    // Find the assistant message with the tool call and check its result.
    bool found_disabled = false;
    for (const auto& msg : session.conversation().messages()) {
        for (const auto& tc : msg.tool_calls) {
            if (tc.name == "read_file" && tc.result.find("disabled") != std::string::npos) {
                found_disabled = true;
            }
        }
    }
    CHECK(found_disabled);

    // The mock should have been called three times:
    // 1. model discovery
    // 2. original tool call request
    // 3. follow-up after tool "result" (the disabled error)
    CHECK(call_count == 3);
}
