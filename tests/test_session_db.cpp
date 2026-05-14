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
    // Tables should be initialized in constructor
    CHECK(db.message_count() == 0);

    // query_session should be able to see the tables
    auto result = db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
    REQUIRE(result);
    auto tables = json::parse(*result);
    REQUIRE(tables.is_array());
    bool found_messages = false;
    bool found_tool_calls = false;
    for (const auto& t : tables) {
        std::string name = t["name"];
        if (name == "messages") found_messages = true;
        if (name == "tool_calls") found_tool_calls = true;
    }
    CHECK(found_messages);
    CHECK(found_tool_calls);
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
    // reasoning_content should NOT appear when empty
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
    {
        ToolCall tc;
        tc.index = 0;
        tc.id = "call_req1";
        tc.name = "list_files";
        tc.arguments = R"({"path": "."})";
        db.add_assistant("", "", {tc});
    }
    db.add_tool("call_req1", "file1.txt\nfile2.txt");

    json msgs = db.build_openai_payload("You are helpful.");
    REQUIRE(msgs.size() == 4);

    CHECK(msgs[3]["role"] == "tool");
    CHECK(msgs[3]["tool_call_id"] == "call_req1");
    CHECK(msgs[3]["content"] == "file1.txt\nfile2.txt");
}

TEST_CASE("SessionDB empty content for tool_call message is null", "[session_db]") {
    SessionDB db;

    db.add_user("hi");
    // Simulate a tool_call assistant message with reasoning but no content
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

    // Simulate error rollback
    db.truncate_conversation(snapshot);
    CHECK(db.message_count() == 0);
}

TEST_CASE("SessionDB estimate total tokens", "[session_db]") {
    SessionDB db;

    // Empty conversation should have low token count
    size_t empty_tokens = db.estimate_total_tokens();

    db.add_user("Hello world");
    db.add_assistant("Hi there!");

    size_t filled_tokens = db.estimate_total_tokens();
    CHECK(filled_tokens > empty_tokens);
    CHECK(filled_tokens >= 10); // at least some tokens
}

TEST_CASE("SessionDB prune droppable", "[session_db]") {
    SessionDB db;

    db.add_user("List files");
    {
        ToolCall tc;
        tc.id = "call_1";
        tc.name = "list_files";
        tc.arguments = R"({"path": "."})";
        db.add_assistant("", "", {tc});
    }
    db.add_tool("call_1", "file1.txt");
    db.add_assistant("Here are the files.");

    CHECK(db.message_count() == 4);

    // Prune droppable (tool results are tagged 'droppable')
    db.prune_droppable();

    // The tool result (droppable) is removed, and the orphaned assistant
    // tool-call message (no remaining tool results for its calls) is also removed.
    // User + final content assistant remain.
    CHECK(db.message_count() == 2);

    // Verify the tool result and orphaned assistant are gone
    json msgs = db.build_openai_payload("test");
    REQUIRE(msgs.size() == 3); // system + 2 remaining messages
    CHECK(msgs[1]["role"] == "user");
    CHECK(msgs[2]["role"] == "assistant");
    CHECK(msgs[2]["content"] == "Here are the files.");
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
