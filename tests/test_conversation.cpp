#include "conversation.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========================================================================
// Conversation conversation management
// ========================================================================

TEST_CASE("Conversation empty by default", "[conversation]") {
    Conversation conv;
    CHECK(conv.message_count() == 0);
}

TEST_CASE("Conversation basic user-assistant exchange", "[conversation]") {
    Conversation conv;

    conv.add_user("Hello");
    conv.add_assistant("Hi there!");

    CHECK(conv.message_count() == 2);

    json msgs = conv.build_openai_payload("You are helpful.");
    REQUIRE(msgs.is_array());
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[0]["role"] == "system");
    CHECK(msgs[0]["content"] == "You are helpful.");

    CHECK(msgs[1]["role"] == "user");
    CHECK(msgs[1]["content"] == "Hello");

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "Hi there!");
    CHECK(msgs[2].find("reasoning_content") == msgs[2].end());
}

TEST_CASE("Conversation with reasoning content", "[conversation]") {
    Conversation conv;

    conv.add_user("What is 2+2?");
    conv.add_assistant("4", "The user asked a simple arithmetic question.");

    json msgs = conv.build_openai_payload("Be brief.");
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "4");
    CHECK(msgs[2]["reasoning_content"] == "The user asked a simple arithmetic question.");
}

TEST_CASE("Conversation with tool calls", "[conversation]") {
    Conversation conv;

    conv.add_user("List files in /tmp");

    ToolCall tc;
    tc.index = 0;
    tc.id = "call_abc123";
    tc.name = "list_path";
    tc.arguments = R"({"path": "/tmp"})";

    auto mid = conv.add_assistant("", "Need to list /tmp contents.", {tc});
    // Every tool_call must have a corresponding tool result (even if empty).
    conv.add_tool(mid, "call_abc123", "");

    json msgs = conv.build_openai_payload("You are helpful.");
    // system + user + assistant + tool(empty result)
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == nullptr);
    CHECK(msgs[2]["reasoning_content"] == "Need to list /tmp contents.");

    auto tcs = msgs[2]["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["id"] == "call_abc123");
    CHECK(tcs[0]["type"] == "function");
    CHECK(tcs[0]["function"]["name"] == "list_path");
    CHECK(tcs[0]["function"]["arguments"] == R"({"path": "/tmp"})");
}

TEST_CASE("Conversation with tool result", "[conversation]") {
    Conversation conv;

    conv.add_user("List files");
    int64_t msg_id;
    {
        ToolCall tc;
        tc.index = 0;
        tc.id = "call_req1";
        tc.name = "list_path";
        tc.arguments = R"({"path": "."})";
        msg_id = conv.add_assistant("", "", {tc});
    }
    conv.add_tool(msg_id, "call_req1", "file1.txt\nfile2.txt");

    json msgs = conv.build_openai_payload("You are helpful.");
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "call_req1");
    CHECK(msgs[3]["content"] == "file1.txt\nfile2.txt");
}

TEST_CASE("Conversation empty tool result is still emitted", "[conversation]") {
    Conversation conv;

    conv.add_user("Run a silent command");

    ToolCall tc;
    tc.id = "call_empty";
    tc.name = "run_bash";
    tc.arguments = R"({"command": "true"})";
    auto mid = conv.add_assistant("", "", {tc});
    conv.add_tool(mid, "call_empty", "");  // empty result

    json msgs = conv.build_openai_payload("test");
    // system + user + assistant + tool
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "call_empty");
    // Empty string, not missing — the API requires a tool message for every
    // tool_call_id, even when the result is empty.
    CHECK(msgs[3]["content"] == "");
}

TEST_CASE("Conversation mixed empty and non-empty tool results", "[conversation]") {
    Conversation conv;

    conv.add_user("Do two things");

    ToolCall tc1;
    tc1.id = "empty_call";
    tc1.name = "run_bash";
    tc1.arguments = R"({"command": "true"})";
    ToolCall tc2;
    tc2.id = "full_call";
    tc2.name = "read_file";
    tc2.arguments = R"({"path": "foo.txt"})";

    auto mid = conv.add_assistant("", "", {tc1, tc2});
    conv.add_tool(mid, "empty_call", "");        // empty result
    conv.add_tool(mid, "full_call", "contents");  // non-empty result

    json msgs = conv.build_openai_payload("test");
    // system + user + assistant + tool(empty) + tool(full)
    REQUIRE(msgs.size() == 5);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "empty_call");
    CHECK(msgs[3]["content"] == "");

    CHECK(msgs[4]["role"] == "tool");
    CHECK(msgs[4]["tool_call_id"] == "full_call");
    CHECK(msgs[4]["content"] == "contents");
}

TEST_CASE("Conversation empty content for tool_call message is null", "[conversation]") {
    Conversation conv;

    conv.add_user("hi");
    {
        ToolCall tc;
        tc.id = "c1";
        tc.name = "list_path";
        tc.arguments = "{}";
        conv.add_assistant("", "thinking...", {tc});
    }

    json msgs = conv.build_openai_payload("test");
    CHECK(msgs[2]["content"] == nullptr);
    CHECK(msgs[2]["reasoning_content"] == "thinking...");
}

TEST_CASE("Conversation truncate conversation on error", "[conversation]") {
    Conversation conv;

    auto snapshot = conv.message_count(); // 0
    conv.add_user("User message");
    conv.add_assistant("Thinking...");
    CHECK(conv.message_count() == 2);

    conv.truncate_conversation(snapshot);
    CHECK(conv.message_count() == 0);
}

TEST_CASE("Conversation estimate total tokens", "[conversation]") {
    Conversation conv;

    size_t empty_tokens = conv.estimate_total_tokens();

    conv.add_user("Hello world");
    conv.add_assistant("Hi there!");

    size_t filled_tokens = conv.estimate_total_tokens();
    CHECK(filled_tokens > empty_tokens);
    CHECK(filled_tokens >= 10);
}

TEST_CASE("Conversation replace_with_summary", "[conversation]") {
    Conversation conv;

    conv.add_user("Hello");
    conv.add_assistant("Hi there!");
    conv.add_user("What is Python?");
    conv.add_assistant("A programming language.");

    CHECK(conv.message_count() == 4);

    conv.replace_with_summary("User asked about Python. Assistant explained it is a programming language.");

    CHECK(conv.message_count() == 1);

    json msgs = conv.build_openai_payload("test");
    REQUIRE(msgs.size() == 2);
    CHECK(msgs[1]["role"] == "user");
    CHECK(msgs[1]["content"].get<std::string>().find("User asked about Python") != std::string::npos);
}

TEST_CASE("Conversation multiple tool calls in one message", "[conversation]") {
    Conversation conv;

    conv.add_user("Do multiple things");

    std::vector<ToolCall> calls;
    {
        ToolCall tc1;
        tc1.index = 0;
        tc1.id = "call_1";
        tc1.name = "list_path";
        tc1.arguments = R"({"path": "."})";
        calls.push_back(tc1);
    }
    {
        ToolCall tc2;
        tc2.index = 1;
        tc2.id = "call_2";
        tc2.name = "read_file";
        tc2.arguments = R"({"path": "/tmp/foo.txt"})";
        calls.push_back(tc2);
    }

    auto mid = conv.add_assistant("", "Need to do both.", calls);
    // Every tool_call must have a corresponding tool result (even if empty).
    conv.add_tool(mid, "call_1", "");
    conv.add_tool(mid, "call_2", "");

    json msgs = conv.build_openai_payload("test");
    // system + user + assistant + tool(empty) + tool(empty)
    REQUIRE(msgs.size() == 5);

    auto tcs = msgs[2]["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 2);

    CHECK(tcs[0]["id"] == "call_1");
    CHECK(tcs[0]["function"]["name"] == "list_path");
    CHECK(tcs[0]["function"]["arguments"] == R"({"path": "."})");

    CHECK(tcs[1]["id"] == "call_2");
    CHECK(tcs[1]["function"]["name"] == "read_file");
    CHECK(tcs[1]["function"]["arguments"] == R"({"path": "/tmp/foo.txt"})");
}

TEST_CASE("Conversation multiple tool results in order", "[conversation]") {
    Conversation conv;

    conv.add_user("Do two things");

    std::vector<ToolCall> calls;
    {
        ToolCall tc1;
        tc1.id = "first";
        tc1.name = "list_path";
        tc1.arguments = R"({"path": "."})";
        calls.push_back(tc1);
    }
    {
        ToolCall tc2;
        tc2.id = "second";
        tc2.name = "read_file";
        tc2.arguments = R"({"path": "/tmp/foo.txt"})";
        calls.push_back(tc2);
    }

    int64_t mid = conv.add_assistant("", "", calls);
    conv.add_tool(mid, "first", "file1.txt");
    conv.add_tool(mid, "second", "file contents here");

    json msgs = conv.build_openai_payload("test");
    // system + user + assistant(with 2 calls) + tool(first) + tool(second)
    REQUIRE(msgs.size() == 5);

    // Assistant
    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["tool_calls"].size() == 2);

    // Tool results in the same order as the calls
    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "first");
    CHECK(msgs[3]["content"] == "file1.txt");

    CHECK(msgs[4]["role"] == "tool");
    CHECK(msgs[4]["tool_call_id"] == "second");
    CHECK(msgs[4]["content"] == "file contents here");

    // If we add another assistant, the sequence continues
    conv.add_assistant("Done.");
    msgs = conv.build_openai_payload("test");
    CHECK(msgs.size() == 6);
    CHECK(msgs[5]["role"] == "assistant");
    CHECK(msgs[5]["content"] == "Done.");
}

TEST_CASE("Conversation to_json / from_json round-trip", "[conversation]") {
    Conversation conv;

    conv.add_user("Hello");
    conv.add_assistant("Hi there!");

    ToolCall tc;
    tc.id = "call_1";
    tc.name = "list_path";
    tc.arguments = R"({"path": "."})";
    auto mid = conv.add_assistant("", "", {tc});
    conv.add_tool(mid, "call_1", "file1.txt");

    // Round-trip through JSON
    auto j = conv.to_json();
    Conversation conv2;
    conv2.from_json(j);

    CHECK(conv2.message_count() == 3);

    auto msgs = conv2.build_openai_payload("test");
    // system + user("Hello") + assistant("Hi there!") + assistant(tool_calls) + tool(result)
    REQUIRE(msgs.size() == 5);

    CHECK(msgs[1]["role"] == "user");
    CHECK(msgs[1]["content"] == "Hello");

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "Hi there!");

    CHECK(msgs[3]["role"] == "assistant");
    CHECK(msgs[3]["tool_calls"].size() == 1);
    CHECK(msgs[3]["tool_calls"][0]["id"] == "call_1");

    CHECK(msgs[4]["role"] == "tool");
    CHECK(msgs[4]["content"] == "file1.txt");
}

TEST_CASE("Conversation droppable tokens estimation", "[conversation]") {
    Conversation conv;

    // No tool calls = no droppable tokens
    CHECK(conv.estimate_droppable_tokens() == 0);

    ToolCall tc;
    tc.id = "call_1";
    tc.name = "test";
    tc.arguments = "{}";
    tc.result = std::string(100, 'x'); // 100 bytes of result
    auto mid = conv.add_assistant("", "", {tc});
    conv.add_tool(mid, "call_1", std::string(100, 'x'));

    // Should have droppable tokens now
    CHECK(conv.estimate_droppable_tokens() > 0);
}

TEST_CASE("Conversation clear removes all messages and resets ID counter", "[conversation]") {
    Conversation conv;

    conv.add_user("Hello");
    conv.add_assistant("Hi!");
    CHECK(conv.message_count() == 2);

    conv.clear();
    CHECK(conv.message_count() == 0);

    // Verify next ID is reset to 1
    auto id = conv.add_user("After clear");
    CHECK(id == 1);
}
