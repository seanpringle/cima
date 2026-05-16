#pragma once

#include "lsp/json_rpc.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <poll.h>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

// -------------------------------------------------------------------
// MockLspServer — a lightweight LSP server test double
//
// Spawns a child process that speaks JSON-RPC over stdin/stdout,
// behaving like a minimal clangd.  Designed for testing LspClient
// without requiring clangd to be installed.
//
// Usage:
//   MockLspServer mock;
//   mock.set_diagnostics_response({...});    // configure (optional)
//   mock.set_response_delay(200);            // optional delay
//   CHECK(mock.start());                     // fork child
//
//   auto caps = mock.send_initialize();      // handshake
//   auto resp = mock.send_request(method, params);
//   auto diag = mock.send_pull_diagnostics(uri);
//
//   mock.shutdown();  // or let destructor handle it
// -------------------------------------------------------------------

class MockLspServer {
public:
    struct PushDiag {
        std::string uri;
        json diagnostics; // array of Diagnostic objects
    };

    MockLspServer() = default;

    // Non-copyable, non-movable
    MockLspServer(const MockLspServer&) = delete;
    MockLspServer& operator=(const MockLspServer&) = delete;

    ~MockLspServer() { cleanup(); }

    // ── Configuration (call before start()) ──────────────────────────

    /// Set the diagnostics array returned by textDocument/pullDiagnostics.
    void set_diagnostics_response(json diagnostics) {
        diagnostics_response_ = std::move(diagnostics);
    }

    /// Add an artificial delay (ms) before each response.
    void set_response_delay(int ms) {
        response_delay_ms_ = ms;
    }

    /// Set a push diagnostic notification to be sent after initialized.
    void set_push_diagnostics(PushDiag push) {
        push_diag_ = std::move(push);
    }

    /// Set the canned response for textDocument/hover (null = return nullptr).
    void set_hover_response(json resp) {
        hover_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/definition (null = return nullptr).
    void set_definition_response(json resp) {
        definition_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/completion (null = return nullptr).
    void set_completion_response(json resp) {
        completion_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/codeAction (null = return nullptr).
    void set_code_action_response(json resp) {
        code_action_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/rename (null = return nullptr = not applicable).
    void set_rename_response(json resp) {
        rename_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/prepareRename (null = symbol not renameable).
    void set_prepare_rename_response(json resp) {
        prepare_rename_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/formatting (null = return nullptr).
    void set_formatting_response(json resp) {
        formatting_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/references (null = return nullptr).
    void set_references_response(json resp) {
        references_response_ = std::move(resp);
    }

    /// Set the canned response for textDocument/documentSymbol (null = return nullptr).
    void set_document_symbols_response(json resp) {
        document_symbols_response_ = std::move(resp);
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

    // ── Convenience request helpers ──────────────────────────────────

    /// Send initialize request, return server capabilities (result.capabilities).
    std::optional<json> send_initialize(int timeout_ms = 5000);

    /// Send an arbitrary request and return the full response.
    std::optional<json> send_request(const std::string& method,
                                     json params,
                                     int timeout_ms = 5000);

    /// Send textDocument/pullDiagnostics, return the diagnostics array.
    std::optional<json> send_pull_diagnostics(const std::string& uri,
                                              int timeout_ms = 5000);

    /// Read the next push notification (if any was sent by server).
    /// Blocks until one arrives or timeout.
    std::optional<json> read_push_notification(int timeout_ms = 5000);

    /// Query the last notification the mock server received.
    /// This sends a \_mock_getLastNotification request.
    std::optional<json> last_notification(int timeout_ms = 5000);

    /// Get all notifications the mock server has received since start
    /// or since the last clear. Sends a \_mock_getAllNotifications request.
    std::optional<json> get_received_notifications(int timeout_ms = 5000);

    /// Clear the recorded notifications list in the mock child.
    /// Sends a \_mock_clearNotifications request.
    std::optional<json> clear_notifications(int timeout_ms = 5000);

    // ── Raw pipe access (for LspClient testing) ──────────────────────

    /// File descriptor for writing to the child's stdin.
    int child_stdin() const { return parent_write_fd_; }

    /// File descriptor for reading from the child's stdout.
    int child_stdout() const { return parent_read_fd_; }

private:
    void cleanup();
    void child_main();

    // Write a raw message to the child's stdin.
    bool write_raw(const std::string& data);
    // Read a raw message (JSON-RPC wire format) from child's stdout.
    std::optional<std::string> read_raw(int timeout_ms);

    // Configuration
    json diagnostics_response_ = json::array();
    json hover_response_;          // null by default → returns nullptr from server
    json definition_response_;      // null by default → returns nullptr from server
    json completion_response_;      // null by default → returns nullptr from server
    json code_action_response_;     // null by default → returns nullptr from server
    json rename_response_;          // null by default → returns nullptr (not applicable)
    json prepare_rename_response_;  // null by default → symbol not renameable
    json formatting_response_;      // null by default → returns nullptr
    json references_response_;      // null by default → returns nullptr
    json document_symbols_response_; // null by default → returns nullptr
    int response_delay_ms_ = 0;
    std::optional<PushDiag> push_diag_;

    // Pipes
    int parent_write_fd_ = -1;  // write to child's stdin
    int parent_read_fd_ = -1;   // read from child's stdout

    // Child process
    pid_t child_pid_ = -1;

    // Push notification buffer: messages received from the child
    // that were notifications (not responses to our requests).
    std::mutex notif_mutex_;
    std::queue<json> pending_notifications_;
    std::condition_variable notif_cv_;
};

// ===================================================================
// Implementation
// ===================================================================

inline bool MockLspServer::start() {
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

inline void MockLspServer::child_main() {
    std::string read_buf;
    bool initialized = false;
    bool running = true;
    json last_notification; // tracks the last received notification for testing
    json all_notifications = json::array(); // all received notifications (for tests)

    while (running) {
        // Read more data into buffer
        char tmp[4096];
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0)
            break; // EOF or error

        read_buf.append(tmp, n);

        // Process all complete messages in the buffer
        while (true) {
            auto decoded = lsp::decode_message(read_buf);
            if (!decoded)
                break; // need more data

            read_buf.erase(0, decoded->consumed);
            const json& msg = decoded->message;

            if (lsp::is_request(msg)) {
                std::string method = msg["method"];
                int id = msg["id"];

                // Handle known methods
                json response;
                if (method == "initialize") {
                    response = lsp::make_response(id, json::object({
                        {"capabilities", json::object({
                            {"textDocument", json::object({
                                {"synchronization", json::object({
                                    {"didOpen", true},
                                    {"didChange", true},
                                    {"didClose", true}
                                })},
                                {"diagnostics", true},
                                {"hover", true},
                                {"completion", json::object()},
                                {"codeAction", true},
                                {"definition", true},
                                {"references", true},
                                {"documentSymbol", true},
                                {"rename", true},
                                {"formatting", true}
                            })},
                            {"diagnosticProvider", json::object({
                                {"interFileDependencies", true},
                                {"workspaceDiagnostics", false}
                            })}
                        })},
                        {"serverInfo", json::object({
                            {"name", "MockLspServer"},
                            {"version", "1.0.0"}
                        })}
                    }));
                } else if (method == "shutdown") {
                    response = lsp::make_response(id, nullptr);
                    running = false;
                } else if (method == "textDocument/diagnostic") {
                    response = lsp::make_response(id, json::object({
                        {"kind", "full"},
                        {"items", diagnostics_response_},
                        {"relatedDocuments", json::object()}
                    }));
                } else if (method == "textDocument/hover") {
                    if (hover_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, hover_response_);
                    }
                } else if (method == "textDocument/definition") {
                    if (definition_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, definition_response_);
                    }
                } else if (method == "textDocument/completion") {
                    if (completion_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, completion_response_);
                    }
                } else if (method == "textDocument/codeAction") {
                    if (code_action_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, code_action_response_);
                    }
                } else if (method == "textDocument/prepareRename") {
                    if (prepare_rename_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, prepare_rename_response_);
                    }
                } else if (method == "textDocument/references") {
                    if (references_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, references_response_);
                    }
                } else if (method == "textDocument/documentSymbol") {
                    if (document_symbols_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, document_symbols_response_);
                    }
                } else if (method == "_mock_getLastNotification") {
                    response = lsp::make_response(id, last_notification);
                } else if (method == "_mock_getAllNotifications") {
                    response = lsp::make_response(id, all_notifications);
                } else if (method == "_mock_clearNotifications") {
                    all_notifications = json::array();
                    last_notification = json();
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setHoverResponse") {
                    hover_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setDefinitionResponse") {
                    definition_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setCompletionResponse") {
                    completion_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setCodeActionResponse") {
                    code_action_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setRenameResponse") {
                    rename_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setPrepareRenameResponse") {
                    prepare_rename_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setFormattingResponse") {
                    formatting_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setReferencesResponse") {
                    references_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "_mock_setDocumentSymbolsResponse") {
                    document_symbols_response_ = msg["params"]["response"];
                    response = lsp::make_response(id, true);
                } else if (method == "textDocument/rename") {
                    if (rename_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, rename_response_);
                    }
                } else if (method == "textDocument/formatting" ||
                           method == "textDocument/rangeFormatting") {
                    if (formatting_response_.is_null()) {
                        response = lsp::make_response(id, nullptr);
                    } else {
                        response = lsp::make_response(id, formatting_response_);
                    }
                } else {
                    // Unknown method → MethodNotFound error
                    response = lsp::make_error_response(
                        id, lsp::ErrorCodes::MethodNotFound,
                        "method not found: " + method);
                }

                // Apply delay if configured
                if (response_delay_ms_ > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(response_delay_ms_));
                }

                std::string wire = lsp::encode_message(response);
                ssize_t written = write(STDOUT_FILENO, wire.data(), wire.size());
                (void)written;

            } else if (lsp::is_notification(msg)) {
                std::string method = msg["method"];
                last_notification = msg;
                all_notifications.push_back(msg);
                if (method == "initialized") {
                    initialized = true;
                    // Send push diagnostics if configured
                    if (push_diag_.has_value()) {
                        json notif = lsp::make_notification(
                            "textDocument/publishDiagnostics", json::object({
                                {"uri", push_diag_->uri},
                                {"diagnostics", push_diag_->diagnostics}
                            }));
                        std::string wire = lsp::encode_message(notif);
                        ssize_t written = write(STDOUT_FILENO, wire.data(), wire.size());
                        (void)written;
                    }
                } else if (method == "exit") {
                    running = false;
                }
            }
        }
    }
}

inline void MockLspServer::shutdown() {
    if (child_pid_ <= 0)
        return;

    // Send shutdown request
    json req = lsp::make_request("shutdown", {});
    auto wire = lsp::encode_message(req);
    write_raw(wire);

    // Wait for response (ignore timeout)
    read_raw(2000);

    // Send exit notification
    json exit_notif = lsp::make_notification("exit", {});
    wire = lsp::encode_message(exit_notif);
    write_raw(wire);

    // Wait for child to exit
    int status;
    waitpid(child_pid_, &status, 0);
    child_pid_ = -1;
}

inline void MockLspServer::crash() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGKILL);
        int status;
        waitpid(child_pid_, &status, 0);
        child_pid_ = -1;
    }
}

inline void MockLspServer::cleanup() {
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

// ── Convenience request methods ─────────────────────────────────

inline std::optional<json> MockLspServer::last_notification(int timeout_ms) {
    return send_request("_mock_getLastNotification", {}, timeout_ms);
}

inline std::optional<json> MockLspServer::send_initialize(int timeout_ms) {
    auto resp = send_request("initialize", {
        {"processId", getpid()},
        {"capabilities", json::object()}
    }, timeout_ms);
    if (!resp)
        return std::nullopt;

    // Send initialized notification
    json notif = lsp::make_notification("initialized", {});
    write_raw(lsp::encode_message(notif));

    // Return the capabilities from the result
    auto& result = (*resp)["result"];
    return result;
}

inline std::optional<json> MockLspServer::send_request(
    const std::string& method, json params, int timeout_ms) {
    json req = lsp::make_request(method, std::move(params));
    auto wire = lsp::encode_message(req);

    if (!write_raw(wire))
        return std::nullopt;

    // Read response(s) until we get one matching our request ID
    int expected_id = req["id"];
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining < 0)
            return std::nullopt; // timeout

        auto raw = read_raw(std::min(remaining, 5000L));
        if (!raw)
            return std::nullopt;

        auto decoded = lsp::decode_message(*raw);
        if (!decoded)
            continue;

        const json& msg = decoded->message;

        // If it's a notification, buffer it
        if (lsp::is_notification(msg)) {
            std::lock_guard<std::mutex> lock(notif_mutex_);
            pending_notifications_.push(msg);
            notif_cv_.notify_one();
            continue;
        }

        // If it's a response for our request ID, return it
        if (lsp::is_response(msg) &&
            msg["id"].get<int>() == expected_id) {
            return msg;
        }

        // Response for a different request (shouldn't happen in this
        // sequential mock usage, but could in theory). Buffer it.
        if (lsp::is_response(msg)) {
            std::lock_guard<std::mutex> lock(notif_mutex_);
            pending_notifications_.push(msg);
            notif_cv_.notify_one();
            continue;
        }
    }
}

inline std::optional<json> MockLspServer::send_pull_diagnostics(
    const std::string& uri, int timeout_ms) {
    auto resp = send_request("textDocument/diagnostic", {
        {"textDocument", {{"uri", uri}}}
    }, timeout_ms);
    if (!resp)
        return std::nullopt;

    auto& result = (*resp)["result"];
    if (result.contains("items") && result["items"].is_array())
        return result["items"];
    return json::array();
}

inline std::optional<json> MockLspServer::get_received_notifications(
    int timeout_ms) {
    auto resp = send_request("_mock_getAllNotifications", {}, timeout_ms);
    if (!resp) return std::nullopt;
    auto& result = (*resp)["result"];
    if (result.is_array()) return result;
    return json::array();
}

inline std::optional<json> MockLspServer::clear_notifications(
    int timeout_ms) {
    return send_request("_mock_clearNotifications", {}, timeout_ms);
}

inline std::optional<json> MockLspServer::read_push_notification(
    int timeout_ms) {
    // First check if we already have one buffered
    {
        std::lock_guard<std::mutex> lock(notif_mutex_);
        if (!pending_notifications_.empty()) {
            auto n = std::move(pending_notifications_.front());
            pending_notifications_.pop();
            return n;
        }
    }

    // Read from pipe until we get a notification or timeout
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining < 0)
            return std::nullopt;

        auto raw = read_raw(std::min(remaining, 5000L));
        if (!raw)
            return std::nullopt;

        auto decoded = lsp::decode_message(*raw);
        if (!decoded)
            continue;

        const json& msg = decoded->message;

        // If it's a notification, return it
        if (lsp::is_notification(msg)) {
            return msg;
        }

        // Otherwise buffer it for send_request to pick up
        std::lock_guard<std::mutex> lock(notif_mutex_);
        pending_notifications_.push(msg);
        notif_cv_.notify_one();
    }
}

// ── Raw I/O helpers ────────────────────────────────────────────

inline bool MockLspServer::write_raw(const std::string& data) {
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

inline std::optional<std::string> MockLspServer::read_raw(int timeout_ms) {
    if (parent_read_fd_ < 0)
        return std::nullopt;

    // Use poll() for timeout
    struct pollfd pfd;
    pfd.fd = parent_read_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return std::nullopt; // timeout or error

    // Read whatever is available. We may get multiple messages;
    // the caller is responsible for decoding.
    char buf[65536];
    ssize_t n = read(parent_read_fd_, buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::nullopt;

    return std::string(buf, n);
}
