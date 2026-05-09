#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <sstream>

// ========================================================================
// ToolAccumulator tests
// ========================================================================

TEST_CASE("ToolAccumulator single chunk with all fields", "[types][toolacc]") {
    ToolAccumulator acc;
    json delta = json::parse(R"({
        "tool_calls": [{
            "index": 0,
            "id": "call_abc",
            "type": "function",
            "function": {
                "name": "list_files",
                "arguments": "{\"path\": \"/tmp\"}"
            }
        }]
    })");
    acc.apply(delta);

    REQUIRE(acc.has_calls());
    auto calls = acc.finalize();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].index == 0);
    CHECK(calls[0].id == "call_abc");
    CHECK(calls[0].name == "list_files");
    CHECK(calls[0].arguments == R"({"path": "/tmp"})");
}

TEST_CASE("ToolAccumulator multi-chunk arguments concatenation",
          "[types][toolacc]") {
    ToolAccumulator acc;

    // Chunk 1: id + name + empty args
    acc.apply(json::parse(R"({
        "tool_calls": [{
            "index": 0,
            "id": "call_xyz",
            "function": {
                "name": "read_file",
                "arguments": ""
            }
        }]
    })"));

    // Chunk 2: partial args fragment
    acc.apply(json::parse(R"({
        "tool_calls": [{
            "index": 0,
            "function": {
                "arguments": "{\"path\":"
            }
        }]
    })"));

    // Chunk 3: rest of args
    acc.apply(json::parse(R"({
        "tool_calls": [{
            "index": 0,
            "function": {
                "arguments": " \"/etc/hosts\"}"
            }
        }]
    })"));

    REQUIRE(acc.has_calls());
    auto calls = acc.finalize();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].id == "call_xyz");
    CHECK(calls[0].name == "read_file");
    CHECK(calls[0].arguments == R"({"path": "/etc/hosts"})");
}

TEST_CASE("ToolAccumulator no tool_calls in delta", "[types][toolacc]") {
    ToolAccumulator acc;
    json delta = json::parse(R"({"content": "hello"})");
    acc.apply(delta);
    CHECK_FALSE(acc.has_calls());
}

TEST_CASE("ToolAccumulator empty delta", "[types][toolacc]") {
    ToolAccumulator acc;
    json delta = json::object();
    acc.apply(delta);
    CHECK_FALSE(acc.has_calls());
}

TEST_CASE("ToolAccumulator multiple parallel tool calls", "[types][toolacc]") {
    ToolAccumulator acc;

    // Chunk with two tool call starts
    acc.apply(json::parse(R"({
        "tool_calls": [
            {"index": 0, "id": "call_1", "function": {"name": "list_files", "arguments": "{\"pa"}},
            {"index": 1, "id": "call_2", "function": {"name": "read_file", "arguments": "{\"pat"}}
        ]
    })"));

    // Chunk with continued args
    acc.apply(json::parse(R"({
        "tool_calls": [
            {"index": 0, "function": {"arguments": "th\": \"/tmp\"}"}},
            {"index": 1, "function": {"arguments": "h\": \"/etc/hosts\"}"}}
        ]
    })"));

    auto calls = acc.finalize();
    REQUIRE(calls.size() == 2);

    // Order by index
    auto* c0 = &calls[0];
    auto* c1 = &calls[1];
    if (calls[0].index != 0) {
        std::swap(c0, c1);
    }

    CHECK(c0->id == "call_1");
    CHECK(c0->name == "list_files");
    CHECK(c0->arguments == R"({"path": "/tmp"})");

    CHECK(c1->id == "call_2");
    CHECK(c1->name == "read_file");
    CHECK(c1->arguments == R"({"path": "/etc/hosts"})");
}

// ========================================================================
// SSEParser tests
// ========================================================================

TEST_CASE("SSEParser single complete event", "[types][sse]") {
    std::vector<json> received;
    bool done = false;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = [&]() { done = true; },
        .on_error = [&](const std::string& e) { FAIL(e); },
    });

    parser.feed("data: {\"key\":\"value\"}\n\n", 24);
    REQUIRE(received.size() == 1);
    CHECK(received[0]["key"] == "value");
    CHECK_FALSE(done);
}

TEST_CASE("SSEParser multiple events in one feed", "[types][sse]") {
    std::vector<json> received;
    bool done = false;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = [&]() { done = true; },
        .on_error = [&](const std::string& e) { FAIL(e); },
    });

    parser.feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\ndata: [DONE]\n\n", 46);
    REQUIRE(received.size() == 2);
    CHECK(received[0]["a"] == 1);
    CHECK(received[1]["b"] == 2);
    CHECK(done);
}

TEST_CASE("SSEParser partial data across feed calls", "[types][sse]") {
    std::vector<json> received;
    bool done = false;
    std::string error;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = [&]() { done = true; },
        .on_error = [&](const std::string& e) { error = e; },
    });

    parser.feed("data: {\"k", 9);
    CHECK(received.empty());

    parser.feed("ey\":1}\n\n", 8);
    REQUIRE(received.size() == 1);
    CHECK(received[0]["key"] == 1);
}

TEST_CASE("SSEParser ignores non-data lines", "[types][sse]") {
    std::vector<json> received;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = []() {},
        .on_error = [&](const std::string& e) { FAIL(e); },
    });

    parser.feed("event: test\ndata: {\"x\":1}\n\n", 28);
    REQUIRE(received.size() == 1);
    CHECK(received[0]["x"] == 1);
}

TEST_CASE("SSEParser reset clears state", "[types][sse]") {
    std::vector<json> received;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = []() {},
        .on_error = [&](const std::string& e) { FAIL(e); },
    });

    parser.feed("data: {\"a\":1}", 13);  // incomplete (no \n), stays buffered
    parser.reset();
    parser.feed("data: {\"b\":2}\n\n", 15);

    REQUIRE(received.size() == 1);
    CHECK(received[0]["b"] == 2);
}

TEST_CASE("SSEParser malformed JSON calls on_error", "[types][sse]") {
    bool errored = false;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [](const json&) {},
        .on_done = []() {},
        .on_error = [&](const std::string&) { errored = true; },
    });

    parser.feed("data: {invalid}\n\n", 17);
    CHECK(errored);
}

TEST_CASE("SSEParser flush processes remaining buffered data", "[types][sse]") {
    std::vector<json> received;
    bool done = false;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = [&]() { done = true; },
        .on_error = [&](const std::string& e) { FAIL(e); },
    });

    // Feed partial data: first event is complete (has \n\n), [DONE] lacks trailing \n
    parser.feed("data: {\"key\":\"value\"}\n\ndata: [DONE]", 35);

    // The first event was already processed by line-splitting;
    // "data: [DONE]" has no trailing \n so it stays buffered.
    REQUIRE(received.size() == 1);
    CHECK(received[0]["key"] == "value");
    CHECK_FALSE(done);

    // flush() processes the remaining "[DONE]" line
    parser.flush();

    CHECK(received.size() == 1);  // no new data events
    CHECK(done);                  // [DONE] was processed
}

TEST_CASE("SSEParser flush handles partial non-DONE data", "[types][sse]") {
    std::vector<json> received;
    bool done = false;
    std::string error;

    SSEParser parser(SSEParser::Callbacks{
        .on_data = [&](const json& j) { received.push_back(j); },
        .on_done = [&]() { done = true; },
        .on_error = [&](const std::string& e) { error = e; },
    });

    // A content line without trailing \n (connection closed mid-stream)
    parser.feed("data: {\"msg\":\"hello\"}", 21);
    CHECK(received.empty());  // nothing processed yet

    parser.flush();
    REQUIRE(received.size() == 1);
    CHECK(received[0]["msg"] == "hello");
    CHECK_FALSE(done);
    CHECK(error.empty());
}

TEST_CASE("SSEParser flush with no buffered data is safe", "[types][sse]") {
    SSEParser parser(SSEParser::Callbacks{
        .on_data = [](const json&) {},
        .on_done = []() {},
        .on_error = [](const std::string&) {},
    });

    // flush with empty buffer should not crash
    parser.flush();
    // flush after well-formed complete data should not crash
    parser.feed("data: {\"x\":1}\n\n", 13);
    parser.flush();
    // flush after reset should not crash
    parser.reset();
    parser.flush();
}

// ========================================================================
// Conversation tests
// ========================================================================

TEST_CASE("Conversation basic user-assistant exchange",
          "[types][conversation]") {
    Conversation conv("You are helpful.");

    conv.add_user("Hello");
    conv.add_assistant("Hi there!");

    json msgs = conv.to_openai_messages();
    REQUIRE(msgs.is_array());
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[0]["role"] == "system");
    CHECK(msgs[0]["content"] == "You are helpful.");

    CHECK(msgs[1]["role"] == "user");
    CHECK(msgs[1]["content"] == "Hello");

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "Hi there!");
    // reasoning_content should NOT appear when empty
    CHECK(msgs[2].find("reasoning_content") == msgs[2].end());
}

TEST_CASE("Conversation with reasoning content", "[types][conversation]") {
    Conversation conv("Be brief.");

    conv.add_user("What is 2+2?");
    conv.add_assistant("4", "The user asked a simple arithmetic question.");

    json msgs = conv.to_openai_messages();
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "4");
    // reasoning_content is stored but no longer round-tripped to the API
    CHECK(msgs[2].find("reasoning_content") == msgs[2].end());
}

TEST_CASE("Conversation with tool calls", "[types][conversation]") {
    Conversation conv("You are helpful.");

    conv.add_user("List files in /tmp");

    ToolCall tc;
    tc.index = 0;
    tc.id = "call_abc123";
    tc.name = "list_files";
    tc.arguments = R"({"path": "/tmp"})";

    conv.add_assistant("", "Need to list /tmp contents.", {tc});

    json msgs = conv.to_openai_messages();
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == nullptr);
    // reasoning_content is stored but no longer round-tripped
    CHECK(msgs[2].find("reasoning_content") == msgs[2].end());

    auto tcs = msgs[2]["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["id"] == "call_abc123");
    CHECK(tcs[0]["type"] == "function");
    CHECK(tcs[0]["function"]["name"] == "list_files");
    CHECK(tcs[0]["function"]["arguments"] == R"({"path": "/tmp"})");
}

TEST_CASE("Conversation with tool result", "[types][conversation]") {
    Conversation conv("You are helpful.");

    conv.add_user("List files");
    {
        ToolCall tc;
        tc.index = 0;
        tc.id = "call_req1";
        tc.name = "list_files";
        tc.arguments = R"({"path": "."})";
        conv.add_assistant("", "", {tc});
    }
    conv.add_tool("call_req1", "file1.txt\nfile2.txt");

    json msgs = conv.to_openai_messages();
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "call_req1");
    CHECK(msgs[3]["content"] == "file1.txt\nfile2.txt");
}

TEST_CASE("Conversation clear preserves system prompt", "[types][conversation]") {
    Conversation conv("System prompt.");
    conv.add_user("Hello");
    conv.add_assistant("Hi");
    conv.clear();

    json msgs = conv.to_openai_messages();
    REQUIRE(msgs.size() == 1);
    CHECK(msgs[0]["role"] == "system");
    CHECK(msgs[0]["content"] == "System prompt.");
    CHECK(conv.system_prompt() == "System prompt.");
}

TEST_CASE("Conversation empty content for tool_call message is null",
          "[types][conversation]") {
    Conversation conv("test");
    conv.add_user("hi");
    // Simulate a tool_call assistant message with reasoning but no content
    {
        ToolCall tc;
        tc.id = "c1";
        tc.name = "list_files";
        tc.arguments = "{}";
        conv.add_assistant("", "thinking...", {tc});
    }

    json msgs = conv.to_openai_messages();
    CHECK(msgs[2]["content"] == nullptr);
    // reasoning_content is stored but no longer round-tripped
    CHECK(msgs[2].find("reasoning_content") == msgs[2].end());
}
