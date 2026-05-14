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

// ===================================================================
// Simple Q&A
// ===================================================================

TEST_CASE("ChatSession simple Q&A", "[chat]") {
    MockServer server(
        [](const std::string&) -> std::string {
            return make_content_sse("Hello, World!");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test-model";
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
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

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test-model";
    cfg.reasoning_effort = "high";
    cfg.context_limit = 1000;  // avoid model discovery request
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("Say hi");
    REQUIRE(result);

    auto body = parse_request_body(last_request);
    REQUIRE(!body.is_discarded());
    REQUIRE(body.contains("reasoning_effort"));
    CHECK(body["reasoning_effort"] == "high");
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
                return make_tool_call_sse("list_files", R"({"path": "."})");
            }
            // Second request: return content
            return make_content_sse("I found some files.");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
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
            return make_tool_call_sse("list_files", R"({"path": "."})",
                                      "call_" + std::to_string(call_count));
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";
    cfg.max_tool_iterations = 10;

    ChatSession session(std::move(cfg));
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

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
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
                             {{"name", "list_files"},
                              {"arguments", R"({"path": "."})"}}}}}}}}}}}};
                return "data: " + delta.dump() + "\n\ndata: [DONE]\n\n";
            }
            return make_content_sse("Here is what I found.");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "You are helpful.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
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

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "test";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("Read file");
    REQUIRE(result);
    CHECK(result->content == "Read the file.");
    CHECK(call_count == 2);
}

// ===================================================================
// Clear preserves system prompt
// ===================================================================

TEST_CASE("ChatSession clear preserves model and system", "[chat]") {
    MockServer server(
        [](const std::string&) -> std::string {
            return make_content_sse("ok");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "my-model";
    cfg.system_prompt = "System prompt.";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    CHECK(session.model() == "my-model");

    session.clear();
    // clear shouldn't affect model
    CHECK(session.model() == "my-model");

    auto result = session.run_once("hi");
    REQUIRE(result);
    CHECK(result->content == "ok");
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

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.model = "test";
    cfg.system_prompt = "test";
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg), token);
    auto result = session.run_once("Say hi");
    CHECK_FALSE(result);
    // curl should abort the request because the progress callback sees the
    // cancelled token
    CHECK(result.error().find("curl error") != std::string::npos);
}
