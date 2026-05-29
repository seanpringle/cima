#pragma once

#include "tools.h" // for Tool struct

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <expected>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// McpClient — manages an MCP server connection
//
// Supports two transports:
//   - stdio: fork/exec a server process, communicate over stdin/stdout
//            using newline-delimited JSON-RPC 2.0
//   - streamable-http: POST JSON-RPC messages to an HTTP endpoint
//            (non-streaming, immediate responses)
//
// Thread-safe: all public methods can be called from multiple threads.
// Non-copyable, non-movable.
// ---------------------------------------------------------------------------

class McpClient {
  public:
    McpClient();
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    // ── Connection ───────────────────────────────────────────────────

    /// Fork/exec an MCP server and perform the initialize handshake.
    Result<void> start_stdio(const std::string& command,
        const std::vector<std::string>& args,
        const std::string& cwd = "",
        const std::map<std::string, std::string>& env = {},
        int timeout_sec = 60);

    /// Connect to already-established pipes (for testing).
    /// Takes the read end of the server's stdout, and the write end
    /// of the server's stdin.  Performs the MCP initialize handshake.
    Result<void> connect(int read_fd, int write_fd);

    /// Connect to a Streamable HTTP MCP endpoint.
    /// This performs an immediate initialize handshake to verify connectivity.
    Result<void> start_http(const std::string& url, const std::string& api_key = "", int timeout_sec = 60);

    // ── MCP Lifecycle ────────────────────────────────────────────────

    /// Perform the initialize handshake (called automatically by start_stdio/connect).
    Result<void> initialize();

    /// List tools from the server.
    Result<std::vector<Tool>> list_tools();

    /// Call a tool with the given arguments.
    Result<std::string> call_tool(const std::string& name, const json& arguments);

    /// Graceful shutdown: send shutdown request, then exit notification.
    Result<void> shutdown();

    /// True if the server process is believed to be running.
    bool is_running() const;

    // ── Accessors ────────────────────────────────────────────────────

    /// The server capabilities returned from the initialize response.
    const json& server_capabilities() const { return capabilities_; }

    /// The server info returned from the initialize response.
    const json& server_info() const { return server_info_; }

    /// Register a callback for server push notifications.
    using NotificationCallback = std::function<void(const json&)>;
    void on_notification(NotificationCallback cb) { on_notification_ = std::move(cb); }

    /// Set a cancellation token that will be checked during requests.
    void set_cancelled(std::shared_ptr<std::atomic<bool>> token) { cancelled_ = std::move(token); }

  private:
    // Internal: send a request and wait for the matching response.
    Result<json> send_request(const std::string& method, json params, int timeout_sec = 60);

    // Start the background reader thread.
    void start_reader_thread();
    void reader_thread_main();

    // I/O helpers for newline-delimited JSON.
    bool write_line(const std::string& line);
    std::optional<std::string> read_line(int timeout_ms);

    // HTTP transport: send a JSON-RPC message via POST.
    Result<json> http_request(const std::string& method, json params, int timeout_sec);

    // Stored start parameters (for potential crash recovery).
    std::string start_command_;
    std::vector<std::string> start_args_;
    std::string start_cwd_;
    std::map<std::string, std::string> start_env_;
    int start_timeout_sec_ = 60;

    // HTTP transport parameters.
    std::string http_url_;
    std::string http_api_key_;
    std::string session_id_; // MCP-Session-Id from server (if returned)
    bool http_mode_ = false; // true when using HTTP transport

    // Pipe fds (child's stdin/stdout from our perspective).
    int write_fd_ = -1; // write → child's stdin
    int read_fd_ = -1;  // read  ← child's stdout

    // Child process.
    pid_t child_pid_ = -1;

    // State.
    std::atomic<bool> running_{false};
    json capabilities_;
    json server_info_;

    // Request tracking.
    std::atomic<int> next_request_id_{1};
    std::mutex pending_mutex_;
    std::map<int, std::promise<json>> pending_;

    // Background reader thread.
    std::thread reader_thread_;
    std::atomic<bool> reader_stop_{false};

    // Notification callback.
    NotificationCallback on_notification_;

    // Cancellation token.
    std::shared_ptr<std::atomic<bool>> cancelled_;

    // Write mutex (serialize writes to the pipe).
    std::mutex write_mutex_;

    // Buffer for incomplete reads (split across poll boundaries).
    std::string read_buf_;
    std::mutex read_buf_mutex_;
};

// ---------------------------------------------------------------------------
// MCP wire format helpers (newline-delimited JSON)
// ---------------------------------------------------------------------------

/// Encode a JSON value as a single-line message with trailing newline.
inline std::string mcp_encode(const json& msg) { return msg.dump() + "\n"; }

/// Try to decode one JSON message from a buffer.
/// Returns nullopt if no complete line is available.
struct McpDecoded {
    json message;
    size_t consumed; ///< bytes consumed (including the newline)
};

inline std::optional<McpDecoded> mcp_decode(std::string_view buf) {
    auto nl = buf.find('\n');
    if (nl == std::string_view::npos)
        return std::nullopt;

    auto line = buf.substr(0, nl);
    if (line.empty())
        return std::nullopt; // skip empty lines

    try {
        json msg = json::parse(line);
        return McpDecoded{std::move(msg), nl + 1};
    } catch (...) {
        // Malformed JSON on this line — skip it.
        return McpDecoded{json(), nl + 1};
    }
}
