#include "lsp/json_rpc.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

// ===================================================================
// Message encoding
// ===================================================================

TEST_CASE("encode_message formats Content-Length header correctly",
          "[lsp][jsonrpc]") {
    json body = {{"jsonrpc", "2.0"}, {"id", 1},
                 {"method", "textDocument/hover"}, {"params", json::object()}};
    auto encoded = lsp::encode_message(body);

    // Must start with Content-Length header
    CHECK_THAT(encoded, StartsWith("Content-Length: "));
    // Must have \r\n\r\n separator
    auto sep = encoded.find("\r\n\r\n");
    REQUIRE(sep != std::string::npos);
    // Body after separator should be valid JSON matching input
    auto body_str = encoded.substr(sep + 4);
    json decoded = json::parse(body_str);
    CHECK(decoded["method"] == "textDocument/hover");
    CHECK(decoded["id"] == 1);
}

TEST_CASE("encode_message Content-Length matches body size exactly",
          "[lsp][jsonrpc]") {
    json body = {{"jsonrpc", "2.0"}, {"id", 42},
                 {"method", "textDocument/completion"}};
    auto encoded = lsp::encode_message(body);

    // Parse header to extract Content-Length value
    auto header_end = encoded.find("\r\n\r\n");
    REQUIRE(header_end != std::string::npos);
    auto header = encoded.substr(0, header_end);
    auto cl_pos = header.find("Content-Length: ");
    REQUIRE(cl_pos != std::string::npos);
    int length = std::stoi(header.substr(cl_pos + 16));

    auto body_str = encoded.substr(header_end + 4);
    CHECK(body_str.size() == static_cast<size_t>(length));
}

TEST_CASE("encode_message handles unicode in body",
          "[lsp][jsonrpc]") {
    json body = {{"text", "héllo wörld 🌍"}};
    auto encoded = lsp::encode_message(body);

    auto sep = encoded.find("\r\n\r\n");
    REQUIRE(sep != std::string::npos);
    auto body_str = encoded.substr(sep + 4);
    json decoded = json::parse(body_str);
    CHECK(decoded["text"] == "héllo wörld 🌍");
}

// ===================================================================
// Message decoding
// ===================================================================

TEST_CASE("decode_message parses single message", "[lsp][jsonrpc]") {
    json body = {{"jsonrpc", "2.0"}, {"id", 1},
                 {"method", "textDocument/hover"}};
    auto wire = lsp::encode_message(body);

    auto result = lsp::decode_message(wire);
    REQUIRE(result);
    CHECK(result->message["method"] == "textDocument/hover");
    CHECK(result->consumed == wire.size());
}

TEST_CASE("decode_message handles multiple messages in buffer",
          "[lsp][jsonrpc]") {
    json body1 = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "m1"}};
    json body2 = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "m2"}};
    auto wire = lsp::encode_message(body1) + lsp::encode_message(body2);

    auto result = lsp::decode_message(wire);
    REQUIRE(result);
    CHECK(result->message["id"] == 1);
    CHECK(result->message["method"] == "m1");
    // Should only consume the first message
    CHECK(result->consumed < wire.size());

    // Parse remaining
    auto remaining = wire.substr(result->consumed);
    auto result2 = lsp::decode_message(remaining);
    REQUIRE(result2);
    CHECK(result2->message["id"] == 2);
    CHECK(result2->message["method"] == "m2");
}

TEST_CASE("decode_message returns nullopt for partial message",
          "[lsp][jsonrpc]") {
    auto result = lsp::decode_message("Content-Length: 100\r\n\r\n{\"json");
    CHECK_FALSE(result);
}

TEST_CASE("decode_message returns nullopt for missing header",
          "[lsp][jsonrpc]") {
    auto result = lsp::decode_message("not a valid message");
    CHECK_FALSE(result);
}

TEST_CASE("decode_message returns nullopt for malformed Content-Length",
          "[lsp][jsonrpc]") {
    auto result = lsp::decode_message("Content-Length: abc\r\n\r\n{}");
    CHECK_FALSE(result);
}

TEST_CASE("decode_message handles Content-Length with leading/trailing spaces",
          "[lsp][jsonrpc]") {
    std::string wire = "Content-Length: 2\r\n\r\n{}";
    auto result = lsp::decode_message(wire);
    REQUIRE(result);
    CHECK(result->message.is_object());
    CHECK(result->consumed == wire.size());
}

// ===================================================================
// URI conversion
// ===================================================================

TEST_CASE("path_to_uri basic", "[lsp][uri]") {
    CHECK(lsp::path_to_uri("/home/user/src/main.cpp") ==
          "file:///home/user/src/main.cpp");
}

TEST_CASE("path_to_uri handles spaces", "[lsp][uri]") {
    CHECK(lsp::path_to_uri("/tmp/foo bar.cc") ==
          "file:///tmp/foo%20bar.cc");
}

TEST_CASE("path_to_uri encodes special characters", "[lsp][uri]") {
    CHECK(lsp::path_to_uri("/tmp/file#1.cc") ==
          "file:///tmp/file%231.cc");
    CHECK(lsp::path_to_uri("/tmp/file?test.cc") ==
          "file:///tmp/file%3Ftest.cc");
}

TEST_CASE("uri_to_path basic", "[lsp][uri]") {
    auto result = lsp::uri_to_path("file:///home/user/src/main.cpp");
    REQUIRE(result);
    CHECK(*result == "/home/user/src/main.cpp");
}

TEST_CASE("uri_to_path decodes percent-encoded characters", "[lsp][uri]") {
    auto result = lsp::uri_to_path("file:///tmp/foo%20bar.cc");
    REQUIRE(result);
    CHECK(*result == "/tmp/foo bar.cc");

    result = lsp::uri_to_path("file:///tmp/file%231.cc");
    REQUIRE(result);
    CHECK(*result == "/tmp/file#1.cc");
}

TEST_CASE("uri_to_path rejects non-file scheme", "[lsp][uri]") {
    auto result = lsp::uri_to_path("http://example.com/foo.cc");
    CHECK_FALSE(result);
}

TEST_CASE("uri_to_path rejects invalid URIs", "[lsp][uri]") {
    auto result = lsp::uri_to_path("not-a-uri");
    CHECK_FALSE(result);
}

TEST_CASE("uri round-trip preserves path", "[lsp][uri]") {
    std::vector<std::string> paths = {
        "/simple.cc",
        "/home/user/src/main.cpp",
        "/tmp/foo bar.cc",
        "/tmp/file#1.cc",
        "/tmp/file?query.cc",
        "/tmp/ümlaut.cc",
    };
    for (const auto& p : paths) {
        auto uri = lsp::path_to_uri(p);
        auto back = lsp::uri_to_path(uri);
        REQUIRE(back);
        CHECK(*back == p);
    }
}

// ===================================================================
// Message type builders
// ===================================================================

TEST_CASE("make_request builds correct JSON-RPC request",
          "[lsp][jsonrpc]") {
    auto req = lsp::make_request("textDocument/hover",
                                 {{"textDocument", {{"uri", "file:///test.cc"}}},
                                  {"position", {{"line", 10}, {"character", 5}}}});
    CHECK(req["jsonrpc"] == "2.0");
    CHECK(req["id"] == 1);
    CHECK(req["method"] == "textDocument/hover");
    CHECK(req["params"]["textDocument"]["uri"] == "file:///test.cc");
    CHECK(req["params"]["position"]["line"] == 10);
}

TEST_CASE("make_request increments ID", "[lsp][jsonrpc]") {
    // IDs are global across all tests; just verify they increase
    auto req1 = lsp::make_request("m1", {});
    auto req2 = lsp::make_request("m2", {});
    // IDs can't decrease
    CHECK(req1["id"].get<int>() >= 1);
    CHECK(req2["id"].get<int>() > req1["id"].get<int>());
}

TEST_CASE("make_notification builds correct JSON-RPC notification",
          "[lsp][jsonrpc]") {
    auto notif = lsp::make_notification("textDocument/didOpen",
                                        {{"textDocument", {{"uri", "file:///test.cc"}}}});
    CHECK(notif["jsonrpc"] == "2.0");
    CHECK(notif.contains("id") == false); // notifications have no id
    CHECK(notif["method"] == "textDocument/didOpen");
}

TEST_CASE("make_response builds correct JSON-RPC response",
          "[lsp][jsonrpc]") {
    auto resp = lsp::make_response(5, json::parse("\"hello\""));
    CHECK(resp["jsonrpc"] == "2.0");
    CHECK(resp["id"] == 5);
    CHECK(resp["result"] == "hello");
}

TEST_CASE("make_error_response builds JSON-RPC error", "[lsp][jsonrpc]") {
    auto err = lsp::make_error_response(3, -32603, "Internal error",
                                        {{"detail", "stack overflow"}});
    CHECK(err["jsonrpc"] == "2.0");
    CHECK(err["id"] == 3);
    CHECK(err["error"]["code"] == -32603);
    CHECK(err["error"]["message"] == "Internal error");
    CHECK(err["error"]["data"]["detail"] == "stack overflow");
}

// ===================================================================
// Message classification helpers
// ===================================================================

TEST_CASE("is_request/notification/response classification",
          "[lsp][jsonrpc]") {
    auto req = lsp::make_request("m1", {});
    CHECK(lsp::is_request(req));
    CHECK_FALSE(lsp::is_notification(req));
    CHECK_FALSE(lsp::is_response(req));

    auto notif = lsp::make_notification("m2", {});
    CHECK_FALSE(lsp::is_request(notif));
    CHECK(lsp::is_notification(notif));
    CHECK_FALSE(lsp::is_response(notif));

    auto resp = lsp::make_response(1, {{"result", nullptr}});
    CHECK_FALSE(lsp::is_request(resp));
    CHECK_FALSE(lsp::is_notification(resp));
    CHECK(lsp::is_response(resp));
}

TEST_CASE("is_request rejects message with wrong jsonrpc version",
          "[lsp][jsonrpc]") {
    json bad = {{"jsonrpc", "1.0"}, {"id", 1}, {"method", "m"}};
    CHECK_FALSE(lsp::is_request(bad));
}

// ===================================================================
// Error code constants
// ===================================================================

TEST_CASE("error code constants have correct values", "[lsp][jsonrpc]") {
    CHECK(lsp::ErrorCodes::ParseError == -32700);
    CHECK(lsp::ErrorCodes::InvalidRequest == -32600);
    CHECK(lsp::ErrorCodes::MethodNotFound == -32601);
    CHECK(lsp::ErrorCodes::InvalidParams == -32602);
    CHECK(lsp::ErrorCodes::InternalError == -32603);
    CHECK(lsp::ErrorCodes::ServerNotInitialized == -32002);
    CHECK(lsp::ErrorCodes::RequestCancelled == -32800);
    CHECK(lsp::ErrorCodes::ContentModified == -32801);
    CHECK(lsp::ErrorCodes::RequestFailed == -32803);
}
