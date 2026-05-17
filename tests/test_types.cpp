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

    // finalize() returns calls sorted by index
    CHECK(calls[0].index == 0);
    CHECK(calls[0].id == "call_1");
    CHECK(calls[0].name == "list_files");
    CHECK(calls[0].arguments == R"({"path": "/tmp"})");

    CHECK(calls[1].index == 1);
    CHECK(calls[1].id == "call_2");
    CHECK(calls[1].name == "read_file");
    CHECK(calls[1].arguments == R"({"path": "/etc/hosts"})");
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
// ensure_table_blank_lines
// ========================================================================

TEST_CASE("ensure_table_blank_lines inserts blank before table after text", "[types][table]") {
    std::string s = "text\n| A |";
    ensure_table_blank_lines(s);
    CHECK(s == "text\n\n| A |");
}

TEST_CASE("ensure_table_blank_lines leaves already-blank line unchanged", "[types][table]") {
    std::string s = "text\n\n| A |";
    ensure_table_blank_lines(s);
    CHECK(s == "text\n\n| A |");
}

TEST_CASE("ensure_table_blank_lines leaves multiple blank lines unchanged", "[types][table]") {
    std::string s = "text\n\n\n| A |";
    ensure_table_blank_lines(s);
    CHECK(s == "text\n\n\n| A |");
}

TEST_CASE("ensure_table_blank_lines leaves row continuations unchanged", "[types][table]") {
    std::string s = "| A |\n| B |";
    ensure_table_blank_lines(s);
    CHECK(s == "| A |\n| B |");
}

TEST_CASE("ensure_table_blank_lines only inserts for first row of table", "[types][table]") {
    std::string s = "text\n| A |\n| B |";
    ensure_table_blank_lines(s);
    CHECK(s == "text\n\n| A |\n| B |");
}

TEST_CASE("ensure_table_blank_lines leaves leading newline unchanged", "[types][table]") {
    std::string s = "\n| A |";
    ensure_table_blank_lines(s);
    CHECK(s == "\n| A |");
}

TEST_CASE("ensure_table_blank_lines does nothing with no table", "[types][table]") {
    std::string s = "no table here";
    ensure_table_blank_lines(s);
    CHECK(s == "no table here");
}

TEST_CASE("ensure_table_blank_lines handles empty string", "[types][table]") {
    std::string s;
    ensure_table_blank_lines(s);
    CHECK(s.empty());
}

TEST_CASE("ensure_table_blank_lines two separate tables both get blanks", "[types][table]") {
    std::string s = "a\n| 1 |\n\nb\n| 2 |";
    ensure_table_blank_lines(s);
    CHECK(s == "a\n\n| 1 |\n\nb\n\n| 2 |");
}

TEST_CASE("ensure_table_blank_lines multi-row multi-table", "[types][table]") {
    std::string s = "x\n| 1 |\n| 2 |\n\ny\n| 3 |";
    ensure_table_blank_lines(s);
    CHECK(s == "x\n\n| 1 |\n| 2 |\n\ny\n\n| 3 |");
}
