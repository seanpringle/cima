#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// LSP / JSON-RPC utility functions for the Language Server Protocol.
//
// The LSP uses JSON-RPC 2.0 over stdin/stdout with a HTTP-like header:
//   Content-Length: <N>\r\n
//   \r\n
//   <JSON body of exactly N bytes>
// ---------------------------------------------------------------------------

namespace lsp {

// ── JSON-RPC error codes (common subset) ──────────────────────────────

struct ErrorCodes {
    // JSON-RPC standard errors
    static constexpr int ParseError          = -32700;
    static constexpr int InvalidRequest      = -32600;
    static constexpr int MethodNotFound      = -32601;
    static constexpr int InvalidParams       = -32602;
    static constexpr int InternalError       = -32603;

    // LSP-specific errors (range -32099 to -32000)
    static constexpr int ServerNotInitialized = -32002;

    // LSP 3.17+ errors (range -32899 to -32800)
    static constexpr int RequestFailed       = -32803;
    static constexpr int ServerCancelled     = -32802;
    static constexpr int ContentModified     = -32801;
    static constexpr int RequestCancelled    = -32800;
};

// ── Message encoding / decoding ──────────────────────────────────────

/// Encode a JSON body into the wire format:
///   Content-Length: <N>\r\n\r\n<body>
/// where N is the byte size of the JSON string (UTF-8).
std::string encode_message(const json& body);

/// Result of a successful decode: the parsed message and how many bytes
/// were consumed from the input buffer.
struct DecodedMessage {
    json message;
    size_t consumed = 0;
};

/// Try to decode one JSON-RPC message from the front of a buffer.
/// Returns nullopt if:
///   - The buffer doesn't contain a complete header
///   - Content-Length is missing or unparseable
///   - The body data is incomplete
/// On success, `consumed` indicates how many bytes to advance.
std::optional<DecodedMessage> decode_message(std::string_view buffer);

// ── URI helpers (file:// scheme) ─────────────────────────────────────

/// Convert an absolute filesystem path to a file:// URI.
/// Special characters (space, #, ?, etc.) are percent-encoded.
std::string path_to_uri(const std::string& path);

/// Convert a file:// URI back to an absolute filesystem path.
/// Percent-encoded characters are decoded.
/// Returns nullopt if the URI is not a valid file:// URI.
std::optional<std::string> uri_to_path(const std::string& uri);

// ── Message type builders ─────────────────────────────────────────────

/// Create a JSON-RPC 2.0 Request message.
/// Each call increments an internal counter for `id`.
json make_request(const std::string& method, json params);

/// Create a JSON-RPC 2.0 Request message with an explicit ID.
/// Does not touch the internal counter.  Use this when the caller
/// manages its own ID (e.g. LspClient for multi-threaded safety).
json make_request_raw(int id, const std::string& method, json params);

/// Create a JSON-RPC 2.0 Notification message (no id).
json make_notification(const std::string& method, json params);

/// Create a JSON-RPC 2.0 Response message with a result.
json make_response(int id, json result);

/// Create a JSON-RPC 2.0 Response message with an error.
json make_error_response(int id, int code, const std::string& message,
                         json data = nullptr);

// ── Message classification ───────────────────────────────────────────

/// True if `msg` is a JSON-RPC 2.0 Request (has id, method, no result/error).
bool is_request(const json& msg);

/// True if `msg` is a JSON-RPC 2.0 Notification (no id, has method).
bool is_notification(const json& msg);

/// True if `msg` is a JSON-RPC 2.0 Response (has id, has result or error).
bool is_response(const json& msg);

} // namespace lsp
