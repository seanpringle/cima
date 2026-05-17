#pragma once

#include "mcp/mcp_client.h"  // for mcp_encode, mcp_decode

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

// -------------------------------------------------------------------
// MockMcpServer — a lightweight MCP server test double
//
// Spawns a child process that speaks newline-delimited JSON-RPC 2.0
// over stdin/stdout, behaving like a minimal MCP server.
// Designed for testing McpClient without requiring a real server.
//
// Usage:
//   MockMcpServer mock;
//   mock.set_initialize_response({...});  // configure (optional)
//   mock.set_tools_response({...});       // configure what tools/list returns
//   mock.set_response_delay(200);         // optional delay
//   CHECK(mock.start());                 // fork child
//
//   // Use the child_stdin() / child_stdout() fds with McpClient::connect()
//   // or use the convenience methods:
//   mock.send_initialize();
//   auto tools = mock.send_list_tools();
//   auto result = mock.send_call_tool("calc", {"x": 6, "y": 7});
//
//   mock.shutdown();  // or let destructor handle it
// -------------------------------------------------------------------

class MockMcpServer {
public:
    MockMcpServer() = default;

    // Non-copyable, non-movable
    MockMcpServer(const MockMcpServer&) = delete;
    MockMcpServer& operator=(const MockMcpServer&) = delete;

    ~MockMcpServer() { cleanup(); }

    // ── Configuration (call before start()) ──────────────────────────

    /// Set the result of the initialize request.
    void set_initialize_response(json caps) {
        initialize_response_ = std::move(caps);
    }

    /// Set the tools array returned by tools/list.
    void set_tools_response(json tools_array) {
        tools_response_ = std::move(tools_array);
    }

    /// Set the result for a specific tool call (by tool name).
    void set_tool_call_result(const std::string& name, json result) {
        tool_results_[name] = std::move(result);
    }

    /// Set a generic tool call result (used when no per-name mapping exists).
    void set_default_tool_call_result(json result) {
        default_tool_call_result_ = std::move(result);
    }

    /// Set a tool name that should return an error.
    void set_reject_tool(const std::string& name) {
        rejected_tools_.insert(name);
    }

    /// Add an artificial delay (ms) before each response.
    void set_response_delay(int ms) {
        response_delay_ms_ = ms;
    }

    /// Set a separate delay for the initialize response specifically.
    void set_initialize_delay(int ms) {
        initialize_delay_ms_ = ms;
    }

    // ── Lifecycle ────────────────────────────────────────────────────

    /// Fork the child process.  Returns true on success.
    bool start();

    /// Send shutdown request + exit notification, wait for child to exit.
    void shutdown();

    /// Kill the child process immediately (SIGKILL).
    void crash();

    /// True if the child process is still running.
    bool is_running() const {
        return child_pid_ > 0 && kill(child_pid_, 0) == 0;
    }

    // ── Pipe access (for McpClient testing) ──────────────────────────

    /// File descriptor for writing to the child's stdin.
    int child_stdin() const { return parent_write_fd_; }

    /// File descriptor for reading from the child's stdout.
    int child_stdout() const { return parent_read_fd_; }

    // ── List changed notification ────────────────────────────────────

    /// Tell the mock to send a `notifications/tools/list_changed` notification
    /// to the client (fire-and-forget via pipe).
    void send_list_changed();

    // ── Introspection ────────────────────────────────────────────────

    /// Get the last notification the mock server received (via _mock).
    std::optional<json> last_notification(int timeout_ms = 5000);

    /// Get all notifications the mock server has received.
    std::optional<json> get_received_notifications(int timeout_ms = 5000);

    /// Clear the recorded notifications list.
    std::optional<json> clear_notifications(int timeout_ms = 5000);

private:
    void cleanup();
    void child_main();

    // Write a raw line to the child's stdin.
    bool write_raw(const std::string& data);
    // Read a raw line from child's stdout.
    std::optional<std::string> read_raw(int timeout_ms);

    // Configuration
    json initialize_response_ = json::object({
        {"protocolVersion", "2025-11-25"},
        {"capabilities", json::object({
            {"tools", json::object()}
        })},
        {"serverInfo", json::object({
            {"name", "MockMcpServer"},
            {"version", "1.0"}
        })}
    });
    json tools_response_ = json::array();
    std::map<std::string, json> tool_results_;
    json default_tool_call_result_ = json::object({
        {"content", json::array({
            json::object({{"type", "text"}, {"text", "ok"}})
        })}
    });
    std::set<std::string> rejected_tools_;
    int response_delay_ms_ = 0;
    int initialize_delay_ms_ = 0;

    // Pipes
    int parent_write_fd_ = -1;  // write to child's stdin
    int parent_read_fd_ = -1;   // read from child's stdout

    // Child process
    pid_t child_pid_ = -1;

    // Push notification buffer
    std::mutex notif_mutex_;
    std::queue<json> pending_notifications_;
    std::condition_variable notif_cv_;
};

// ===================================================================
// Implementation
// ===================================================================

inline bool MockMcpServer::start() {
    // Create pipes: parent_write → child's stdin
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)
        return false;

    child_pid_ = fork();
    if (child_pid_ == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (child_pid_ == 0) {
        // ── Child ──
        close(stdin_pipe[1]);  // close write end of stdin pipe
        close(stdout_pipe[0]); // close read end of stdout pipe

        // Redirect stdin
        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        // Redirect stdout
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);

        child_main();
        _exit(0);
    }

    // ── Parent ──
    parent_write_fd_ = stdin_pipe[1];  // write to child's stdin
    parent_read_fd_ = stdout_pipe[0];  // read from child's stdout
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    return true;
}

inline void MockMcpServer::child_main() {
    std::string read_buf;
    bool running = true;
    json last_notification;
    json all_notifications = json::array();

    while (running) {
        // Read more data into buffer
        char tmp[4096];
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0)
            break; // EOF or error

        read_buf.append(tmp, n);

        // Process all complete lines in the buffer
        while (true) {
            // Use mcp_decode from mcp_client.h
            auto decoded = mcp_decode(std::string_view(read_buf));
            if (!decoded)
                break; // need more data

            read_buf.erase(0, decoded->consumed);
            const json& msg = decoded->message;

            // Skip empty/malformed messages
            if (msg.is_null() || msg.empty())
                continue;

            // Handle notification (no id)
            if (!msg.contains("id")) {
                std::string method = msg.value("method", std::string());
                last_notification = msg;
                all_notifications.push_back(msg);

                if (method == "exit") {
                    running = false;
                    break;
                }
                continue;
            }

            // ── Request ──
            int id = msg["id"];
            std::string method = msg.value("method", std::string());

            json response;
            response["jsonrpc"] = "2.0";
            response["id"] = id;

            if (method == "initialize") {
                // Apply separate initialize delay if configured
                if (initialize_delay_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(initialize_delay_ms_));
                }
                response["result"] = initialize_response_;
            } else if (method == "tools/list") {
                response["result"] = json::object({
                    {"tools", tools_response_}
                });
            } else if (method == "tools/call") {
                std::string tool_name = msg["params"].value("name", std::string());

                if (rejected_tools_.count(tool_name)) {
                    response["error"] = json::object({
                        {"code", -32603},
                        {"message", "Tool rejected: " + tool_name}
                    });
                } else {
                    auto it = tool_results_.find(tool_name);
                    if (it != tool_results_.end()) {
                        response["result"] = it->second;
                    } else {
                        response["result"] = default_tool_call_result_;
                    }
                }
            } else if (method == "shutdown") {
                response["result"] = nullptr;
                // Note: we don't set running=false here; the client
                // will send an "exit" notification which triggers that.
            } else if (method == "_mock_sendListChanged") {
                // First, send the notification to the client.
                json notif;
                notif["jsonrpc"] = "2.0";
                notif["method"] = "notifications/tools/list_changed";
                std::string notif_wire = mcp_encode(notif);
                ssize_t n = write(STDOUT_FILENO, notif_wire.data(), notif_wire.size());
                (void)n;

                // Then acknowledge the request.
                response["result"] = true;
            } else if (method == "_mock_getLastNotification") {
                response["result"] = last_notification;
            } else if (method == "_mock_getAllNotifications") {
                response["result"] = all_notifications;
            } else if (method == "_mock_clearNotifications") {
                all_notifications = json::array();
                last_notification = json();
                response["result"] = true;
            } else {
                // Unknown method → MethodNotFound error
                response["error"] = json::object({
                    {"code", -32601},
                    {"message", "Method not found: " + method}
                });
            }

            // Apply delay if configured
            if (response_delay_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(response_delay_ms_));
            }

            std::string wire = mcp_encode(response);
            ssize_t written = write(STDOUT_FILENO, wire.data(), wire.size());
            (void)written;
        }
    }
}

inline void MockMcpServer::shutdown() {
    if (child_pid_ <= 0)
        return;

    // Send shutdown request
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1000;
    req["method"] = "shutdown";
    req["params"] = json::object();
    auto wire = mcp_encode(req);
    write_raw(wire);

    // Wait for response (ignore timeout)
    read_raw(2000);

    // Send exit notification
    json exit_notif;
    exit_notif["jsonrpc"] = "2.0";
    exit_notif["method"] = "exit";
    exit_notif["params"] = json::object();
    wire = mcp_encode(exit_notif);
    write_raw(wire);

    // Wait for child to exit
    int status;
    waitpid(child_pid_, &status, 0);
    child_pid_ = -1;
}

inline void MockMcpServer::crash() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGKILL);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
}

inline void MockMcpServer::cleanup() {
    if (child_pid_ > 0) {
        // Try graceful shutdown first, then kill
        shutdown();
        if (child_pid_ > 0) {
            crash();
        }
    }
    if (parent_write_fd_ >= 0) {
        close(parent_write_fd_);
        parent_write_fd_ = -1;
    }
    if (parent_read_fd_ >= 0) {
        close(parent_read_fd_);
        parent_read_fd_ = -1;
    }
}

// ── Introspection methods ────────────────────────────────────

inline std::optional<json> MockMcpServer::last_notification(int timeout_ms) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1001;
    req["method"] = "_mock_getLastNotification";
    req["params"] = json::object();
    auto wire = mcp_encode(req);
    if (!write_raw(wire))
        return std::nullopt;

    auto raw = read_raw(timeout_ms);
    if (!raw)
        return std::nullopt;
    auto decoded = mcp_decode(*raw);
    if (!decoded)
        return std::nullopt;
    return decoded->message["result"];
}

inline std::optional<json> MockMcpServer::get_received_notifications(int timeout_ms) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1002;
    req["method"] = "_mock_getAllNotifications";
    req["params"] = json::object();
    auto wire = mcp_encode(req);
    if (!write_raw(wire))
        return std::nullopt;

    auto raw = read_raw(timeout_ms);
    if (!raw)
        return std::nullopt;
    auto decoded = mcp_decode(*raw);
    if (!decoded)
        return std::nullopt;
    return decoded->message["result"];
}

inline std::optional<json> MockMcpServer::clear_notifications(int timeout_ms) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1003;
    req["method"] = "_mock_clearNotifications";
    req["params"] = json::object();
    auto wire = mcp_encode(req);
    if (!write_raw(wire))
        return std::nullopt;

    auto raw = read_raw(timeout_ms);
    if (!raw)
        return std::nullopt;
    auto decoded = mcp_decode(*raw);
    if (!decoded)
        return std::nullopt;
    return decoded->message["result"];
}

// ── List changed notification ────────────────────────────────

inline void MockMcpServer::send_list_changed() {
    // Send a _mock_sendListChanged request to the child via pipe.
    // Fire-and-forget: we don't wait for the response because the
    // client's reader thread may consume it from the pipe.
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = 1004;
    req["method"] = "_mock_sendListChanged";
    req["params"] = json::object();
    auto wire = mcp_encode(req);
    write_raw(wire);
}

// ── Raw I/O helpers ──────────────────────────────────────────

inline bool MockMcpServer::write_raw(const std::string& data) {
    if (parent_write_fd_ < 0)
        return false;

    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = write(parent_write_fd_, ptr, remaining);
        if (n <= 0)
            return false;
        ptr += n;
        remaining -= n;
    }
    return true;
}

inline std::optional<std::string> MockMcpServer::read_raw(int timeout_ms) {
    if (parent_read_fd_ < 0)
        return std::nullopt;

    // Use poll() for timeout
    struct pollfd pfd;
    pfd.fd = parent_read_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return std::nullopt; // timeout or error

    char buf[65536];
    ssize_t n = read(parent_read_fd_, buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::nullopt;

    return std::string(buf, n);
}
