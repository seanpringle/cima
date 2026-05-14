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

    ChatSession session(std::move(cfg), nullptr, token);
    auto result = session.run_once("Say hi");
    CHECK_FALSE(result);
    // curl should abort the request because the progress callback sees the
    // cancelled token
    CHECK(result.error().find("curl error") != std::string::npos);
}

// ===================================================================
// Usage notice injection
// ===================================================================

TEST_CASE("ChatSession context warning injected at low context_limit", "[chat][notices]") {
    // The first request is always the model-discovery GET (which returns
    // a tool-call SSE — the body is ignored by the discovery parser).
    // The second request is the first chat POST: we make it return a tool
    // call so that the tool executes and the notice injection runs.
    // The third request is the second chat POST carrying the tool result,
    // which should contain the notice.
    int req_idx = 0;
    std::vector<std::string> requests;
    MockServer server(
        [&](const std::string& req) -> std::string {
            requests.push_back(req);
            req_idx++;
            if (req_idx <= 2) {
                // Request 1 = model discovery, Request 2 = first chat
                return make_tool_call_sse("list_files",
                    R"({"path": "."})", "call_abc");
            }
            // Request 3+ = subsequent chat requests → return content
            return make_content_sse("Final answer.");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    // Tiny context so even a short conversation exceeds 60%
    cfg.system_prompt = "test";
    cfg.context_limit = 30;
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("List files");
    REQUIRE(result);
    // Expect at least 3 requests: model discovery, first chat, second chat
    REQUIRE(requests.size() >= 3);

    // The third request (index 2) is the second chat POST, which carries
    // the tool result from the first iteration. Check for the notice.
    auto body = parse_request_body(requests[2]);
    REQUIRE(!body.is_discarded());
    auto msgs = body["messages"];
    REQUIRE(msgs.is_array());

    bool found_notice = false;
    for (const auto& msg : msgs) {
        if (msg["role"] == "tool") {
            std::string content = msg["content"].get<std::string>();
            if (content.find("[context warning:") != std::string::npos) {
                found_notice = true;
                CHECK(content.find("~") != std::string::npos);
                break;
            }
        }
    }
    CHECK(found_notice);
}

TEST_CASE("ChatSession context critical injected at extreme context_limit", "[chat][notices]") {
    int req_idx = 0;
    std::vector<std::string> requests;
    MockServer server(
        [&](const std::string& req) -> std::string {
            requests.push_back(req);
            req_idx++;
            if (req_idx <= 2) {
                return make_tool_call_sse("list_files",
                    R"({"path": "."})", "call_abc");
            }
            return make_content_sse("Done.");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "test";
    cfg.context_limit = 10;  // extremely small -> >90% usage
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("List files");
    REQUIRE(result);
    REQUIRE(requests.size() >= 3);

    // Third request carries the tool result.
    auto body = parse_request_body(requests[2]);
    REQUIRE(!body.is_discarded());
    auto msgs = body["messages"];
    REQUIRE(msgs.is_array());

    bool found_critical = false;
    for (const auto& msg : msgs) {
        if (msg["role"] == "tool") {
            std::string content = msg["content"].get<std::string>();
            if (content.find("[context critical:") != std::string::npos) {
                found_critical = true;
                break;
            }
        }
    }
    CHECK(found_critical);
}

TEST_CASE("ChatSession tool call warning at high iteration budget usage", "[chat][notices]") {
    // With max_tool_iterations=5, the 4th iteration should trigger
    // the tool-call warning (4/5 = 80%, >= 60%).

    // We'll set context_limit very high so no context warning fires,
    // isolating the tool-call warning test.
    std::vector<std::string> requests;
    int call_idx = 0;
    MockServer server(
        [&](const std::string& req) -> std::string {
            requests.push_back(req);
            call_idx++;
            return make_tool_call_sse("list_files",
                R"({"path": "."})",
                "call_" + std::to_string(call_idx));
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "test";
    cfg.context_limit = 300000;  // high enough to avoid context notices
    cfg.max_tool_iterations = 5;
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("List files");
    REQUIRE(result);
    // The budget is 5 iterations, so there should be 6 requests
    // (1 model discovery + 5 iterations). Since every iteration
    // returns a tool call, the last one also returns a tool call
    // and then run_once breaks with budget exhausted.
    // Actually with 5 iterations the last iteration (iter=4) will
    // be 4/5 = 80% which should trigger the warning.
    // But the tool call is processed during iteration 4, and the
    // result is stored, then since no more iterations, the loop
    // falls through to the "budget exhausted" handling.

    // The tool-call warning should appear somewhere >= iteration 3
    // (which is iter=3, 3/5=60%).
    // Let's check the last few requests.
    bool found_warning = false;
    // Check from request 2 onward (request 1 is the first API call)
    for (size_t i = 2; i < requests.size(); i++) {
        auto body = parse_request_body(requests[i]);
        if (body.is_discarded()) continue;
        auto msgs = body["messages"];
        if (!msgs.is_array()) continue;
        for (const auto& msg : msgs) {
            if (msg["role"] == "tool") {
                std::string content = msg["content"].get<std::string>();
                if (content.find("[usage warning:") != std::string::npos) {
                    found_warning = true;
                    break;
                }
            }
        }
        if (found_warning) break;
    }
    CHECK(found_warning);
}

TEST_CASE("ChatSession notice not injected when below thresholds", "[chat][notices]") {
    int req_idx = 0;
    std::vector<std::string> requests;
    MockServer server(
        [&](const std::string& req) -> std::string {
            requests.push_back(req);
            req_idx++;
            if (req_idx <= 2) {
                return make_tool_call_sse("list_files",
                    R"({"path": "."})", "call_abc");
            }
            return make_content_sse("Final answer.");
        },
        true);

    Config cfg;
    cfg.api_base = server.base_url();
    cfg.api_key = "";
    cfg.model = "test";
    cfg.system_prompt = "test";
    cfg.context_limit = 300000;  // very high -> <1% usage
    cfg.safe_dir = "/tmp";

    ChatSession session(std::move(cfg));
    auto result = session.run_once("List files");
    REQUIRE(result);
    REQUIRE(requests.size() >= 3);

    // Third request carries the tool result.
    auto body = parse_request_body(requests[2]);
    REQUIRE(!body.is_discarded());
    auto msgs = body["messages"];
    REQUIRE(msgs.is_array());

    bool has_notice = false;
    for (const auto& msg : msgs) {
        if (msg["role"] == "tool") {
            std::string content = msg["content"].get<std::string>();
            if (content.find('[') != std::string::npos &&
                (content.find("context") != std::string::npos ||
                 content.find("usage") != std::string::npos)) {
                has_notice = true;
                break;
            }
        }
    }
    CHECK_FALSE(has_notice);
}
