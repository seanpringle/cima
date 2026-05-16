#include "lsp/json_rpc.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace lsp {

// ===================================================================
// Message encoding
// ===================================================================

std::string encode_message(const json& body) {
    auto body_str = body.dump(); // nlohmann_json outputs compact UTF-8 by default
    std::string header = "Content-Length: " + std::to_string(body_str.size()) +
                         "\r\n\r\n";
    return header + body_str;
}

// ===================================================================
// Message decoding
// ===================================================================

namespace {

/// Find "\r\n\r\n" separator in a buffer. Returns position just past it
/// (i.e. the start of the body), or npos if not found.
size_t find_header_body_separator(std::string_view buf) {
    auto pos = buf.find("\r\n\r\n");
    if (pos == std::string_view::npos)
        return std::string_view::npos;
    return pos + 4; // skip past the separator
}

/// Parse Content-Length value from the header portion.
/// Returns -1 on error.
int parse_content_length(std::string_view header) {
    // Lines look like: "Content-Length: 123\r\n"
    auto pos = header.find("Content-Length:");
    if (pos == std::string_view::npos)
        return -1;

    pos += 15; // skip past "Content-Length:"
    // Skip whitespace
    while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t'))
        pos++;

    // Read digits until \r, \n, or end
    const char* start = header.data() + pos;
    const char* end = header.data() + header.size();
    // Stop at \r or \n
    auto cr = std::find(start, end, '\r');
    auto lf = std::find(start, end, '\n');
    end = std::min(cr, lf);

    if (start >= end)
        return -1;

    int value = 0;
    auto [ptr, ec] = std::from_chars(start, end, value);
    if (ec != std::errc())
        return -1;
    return value;
}

} // anonymous namespace

std::optional<DecodedMessage> decode_message(std::string_view buffer) {
    // Find the \r\n\r\n separator between header and body
    auto body_start = find_header_body_separator(buffer);
    if (body_start == std::string_view::npos)
        return std::nullopt; // header not complete yet

    auto header = buffer.substr(0, body_start - 4); // exclude \r\n\r\n
    int content_length = parse_content_length(header);
    if (content_length < 0)
        return std::nullopt; // malformed header

    auto body_view = buffer.substr(body_start);
    if (body_view.size() < static_cast<size_t>(content_length))
        return std::nullopt; // body not complete yet

    auto body_str = body_view.substr(0, content_length);
    json message;
    try {
        message = json::parse(body_str);
    } catch (const json::parse_error&) {
        return std::nullopt; // invalid JSON
    }

    return DecodedMessage{
        .message = std::move(message),
        .consumed = body_start + static_cast<size_t>(content_length),
    };
}

// ===================================================================
// URI helpers
// ===================================================================

namespace {

bool is_unreserved(char c) {
    // RFC 3986 unreserved characters: ALPHA / DIGIT / "-" / "." / "_" / "~"
    return std::isalnum(static_cast<unsigned char>(c)) ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

} // anonymous namespace

std::string path_to_uri(const std::string& path) {
    // file:// + absolute path with percent-encoding
    std::string uri = "file://";
    for (unsigned char c : path) {
        if (is_unreserved(c) || c == '/') {
            uri += static_cast<char>(c);
        } else {
            // Percent-encode
            static const char hex[] = "0123456789ABCDEF";
            uri += '%';
            uri += hex[c >> 4];
            uri += hex[c & 0xF];
        }
    }
    return uri;
}

std::optional<std::string> uri_to_path(const std::string& uri) {
    // Must start with file://
    if (uri.size() < 7 || uri.substr(0, 7) != "file://")
        return std::nullopt;

    std::string path;
    path.reserve(uri.size());
    for (size_t i = 7; i < uri.size();) {
        unsigned char c = static_cast<unsigned char>(uri[i]);
        if (c == '%' && i + 2 < uri.size()) {
            // Decode percent-encoded byte
            char hex[3] = {uri[i + 1], uri[i + 2], '\0'};
            char* end = nullptr;
            long val = std::strtol(hex, &end, 16);
            if (end != hex + 2)
                return std::nullopt; // malformed percent encoding
            path += static_cast<char>(val);
            i += 3;
        } else {
            path += static_cast<char>(c);
            i += 1;
        }
    }
    return path;
}

// ===================================================================
// Message type builders
// ===================================================================

namespace {
    std::atomic<int> g_next_id{1};
}

json make_request(const std::string& method, json params) {
    int id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    return make_request_raw(id, method, std::move(params));
}

json make_request_raw(int id, const std::string& method, json params) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", std::move(params)},
    };
}

json make_notification(const std::string& method, json params) {
    return {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
    };
}

json make_response(int id, json result) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}

json make_error_response(int id, int code, const std::string& message,
                         json data) {
    json err = {
        {"code", code},
        {"message", message},
    };
    if (!data.is_null()) {
        err["data"] = std::move(data);
    }
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", std::move(err)},
    };
}

// ===================================================================
// Message classification
// ===================================================================

bool is_request(const json& msg) {
    // A Request MUST have: jsonrpc == "2.0", id, method
    // MUST NOT have: result, error
    auto it = msg.find("jsonrpc");
    if (it == msg.end() || !it->is_string() || *it != "2.0")
        return false;
    if (!msg.contains("id") || !msg.contains("method"))
        return false;
    if (msg.contains("result") || msg.contains("error"))
        return false;
    return true;
}

bool is_notification(const json& msg) {
    // A Notification MUST have: jsonrpc == "2.0", method
    // MUST NOT have: id
    auto it = msg.find("jsonrpc");
    if (it == msg.end() || !it->is_string() || *it != "2.0")
        return false;
    if (!msg.contains("method"))
        return false;
    if (msg.contains("id"))
        return false;
    return true;
}

bool is_response(const json& msg) {
    // A Response MUST have: jsonrpc == "2.0", id
    // MUST have either result or error
    auto it = msg.find("jsonrpc");
    if (it == msg.end() || !it->is_string() || *it != "2.0")
        return false;
    if (!msg.contains("id"))
        return false;
    if (!msg.contains("result") && !msg.contains("error"))
        return false;
    return true;
}

} // namespace lsp
