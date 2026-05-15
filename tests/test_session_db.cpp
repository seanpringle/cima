#include "session_db.h"
#include "types.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// ========================================================================
// SessionDB conversation management
// ========================================================================

TEST_CASE("SessionDB init tables and empty conversation", "[session_db]") {
    SessionDB db;
    CHECK(db.message_count() == 0);

    // query_session should be able to see the tables
    auto result = db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    REQUIRE(result);
    auto tables = json::parse(*result);
    REQUIRE(tables.is_array());
    bool found_messages = false;
    bool found_metadata = false;
    for (const auto& t : tables) {
        std::string name = t["name"];
        if (name == "messages") found_messages = true;
        if (name == "metadata") found_metadata = true;
    }
    CHECK(found_messages);
    CHECK(found_metadata);
}

TEST_CASE("SessionDB basic user-assistant exchange", "[session_db]") {
    SessionDB db;

    db.add_user("Hello");
    db.add_assistant("Hi there!");

    CHECK(db.message_count() == 2);

    json msgs = db.build_openai_payload("You are helpful.");
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

TEST_CASE("SessionDB with reasoning content", "[session_db]") {
    SessionDB db;

    db.add_user("What is 2+2?");
    db.add_assistant("4", "The user asked a simple arithmetic question.");

    json msgs = db.build_openai_payload("Be brief.");
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "4");
    CHECK(msgs[2]["reasoning_content"] == "The user asked a simple arithmetic question.");
}

TEST_CASE("SessionDB with tool calls", "[session_db]") {
    SessionDB db;

    db.add_user("List files in /tmp");

    ToolCall tc;
    tc.index = 0;
    tc.id = "call_abc123";
    tc.name = "list_files";
    tc.arguments = R"({"path": "/tmp"})";

    db.add_assistant("", "Need to list /tmp contents.", {tc});

    json msgs = db.build_openai_payload("You are helpful.");
    REQUIRE(msgs.size() == 3);

    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == nullptr);
    CHECK(msgs[2]["reasoning_content"] == "Need to list /tmp contents.");

    auto tcs = msgs[2]["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["id"] == "call_abc123");
    CHECK(tcs[0]["type"] == "function");
    CHECK(tcs[0]["function"]["name"] == "list_files");
    CHECK(tcs[0]["function"]["arguments"] == R"({"path": "/tmp"})");
}

TEST_CASE("SessionDB with tool result", "[session_db]") {
    SessionDB db;

    db.add_user("List files");
    int64_t msg_id;
    {
        ToolCall tc;
        tc.index = 0;
        tc.id = "call_req1";
        tc.name = "list_files";
        tc.arguments = R"({"path": "."})";
        msg_id = db.add_assistant("", "", {tc});
    }
    db.add_tool(msg_id, "call_req1", "file1.txt\nfile2.txt");

    json msgs = db.build_openai_payload("You are helpful.");
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "call_req1");
    CHECK(msgs[3]["content"] == "file1.txt\nfile2.txt");
}

TEST_CASE("SessionDB empty content for tool_call message is null", "[session_db]") {
    SessionDB db;

    db.add_user("hi");
    {
        ToolCall tc;
        tc.id = "c1";
        tc.name = "list_files";
        tc.arguments = "{}";
        db.add_assistant("", "thinking...", {tc});
    }

    json msgs = db.build_openai_payload("test");
    CHECK(msgs[2]["content"] == nullptr);
    CHECK(msgs[2]["reasoning_content"] == "thinking...");
}

TEST_CASE("SessionDB truncate conversation on error", "[session_db]") {
    SessionDB db;

    auto snapshot = db.message_count(); // 0
    db.add_user("User message");
    db.add_assistant("Thinking...");
    CHECK(db.message_count() == 2);

    db.truncate_conversation(snapshot);
    CHECK(db.message_count() == 0);
}

TEST_CASE("SessionDB estimate total tokens", "[session_db]") {
    SessionDB db;

    size_t empty_tokens = db.estimate_total_tokens();

    db.add_user("Hello world");
    db.add_assistant("Hi there!");

    size_t filled_tokens = db.estimate_total_tokens();
    CHECK(filled_tokens > empty_tokens);
    CHECK(filled_tokens >= 10);
}

TEST_CASE("SessionDB delete assistant removes tool_data cleanly", "[session_db]") {
    SessionDB db;

    db.add_user("List files");
    int64_t msg_id;
    {
        ToolCall tc;
        tc.id = "call_1";
        tc.name = "list_files";
        tc.arguments = R"({"path": "."})";
        msg_id = db.add_assistant("", "", {tc});
    }
    db.add_tool(msg_id, "call_1", "file1.txt");
    int64_t final_id = db.add_assistant("Here are the files.");

    // tool_data is embedded in the assistant — no separate tool row
    CHECK(db.message_count() == 3);

    // Delete the tool-call assistant — tool_data goes with it
    auto result = db.execute("DELETE FROM messages WHERE id = " + std::to_string(msg_id));
    REQUIRE(result);
    CHECK(db.message_count() == 2);

    // Remaining: user, final assistant (tool_calls and results fully gone)
    json msgs = db.build_openai_payload("test");
    REQUIRE(msgs.size() == 3); // system + 2 remaining messages
}

TEST_CASE("SessionDB multiple tool calls in one message", "[session_db]") {
    SessionDB db;

    db.add_user("Do multiple things");

    std::vector<ToolCall> calls;
    {
        ToolCall tc1;
        tc1.index = 0;
        tc1.id = "call_1";
        tc1.name = "list_files";
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

    db.add_assistant("", "Need to do both.", calls);

    json msgs = db.build_openai_payload("test");
    REQUIRE(msgs.size() == 3);

    auto tcs = msgs[2]["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 2);

    CHECK(tcs[0]["id"] == "call_1");
    CHECK(tcs[0]["function"]["name"] == "list_files");
    CHECK(tcs[0]["function"]["arguments"] == R"({"path": "."})");

    CHECK(tcs[1]["id"] == "call_2");
    CHECK(tcs[1]["function"]["name"] == "read_file");
    CHECK(tcs[1]["function"]["arguments"] == R"({"path": "/tmp/foo.txt"})");
}

TEST_CASE("SessionDB multiple tool results in order", "[session_db]") {
    SessionDB db;

    db.add_user("Do two things");

    std::vector<ToolCall> calls;
    {
        ToolCall tc1;
        tc1.id = "first";
        tc1.name = "list_files";
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

    int64_t mid = db.add_assistant("", "", calls);
    db.add_tool(mid, "first", "file1.txt");
    db.add_tool(mid, "second", "file contents here");

    json msgs = db.build_openai_payload("test");
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
    db.add_assistant("Done.");
    msgs = db.build_openai_payload("test");
    CHECK(msgs.size() == 6);
    CHECK(msgs[5]["role"] == "assistant");
    CHECK(msgs[5]["content"] == "Done.");
}
