#pragma once

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

template <typename T> using Result = std::expected<T, std::string>;

// ---------------------------------------------------------------------------
// LspClient — manages a language server subprocess (e.g. clangd)
//
// Communicates over stdin/stdout using JSON-RPC 2.0 with Content-Length
// headers (the LSP transport).  A background reader thread dispatches
// responses to the correct waiting caller via ID-matched promises.
//
// Thread-safe: all public methods can be called from multiple threads.
// ---------------------------------------------------------------------------

class LspClient {
public:
    LspClient();
    ~LspClient();

    // Non-copyable, non-movable
    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;

    // ── Connection ───────────────────────────────────────────────────

    /// Fork/exec a server binary (e.g. clangd) and perform the LSP
    /// initialize handshake.
    Result<void> start(const std::string& binary_path,
                       const std::vector<std::string>& args,
                       const std::string& project_root);

    /// Connect to already-established pipes (for testing).
    /// Takes the read end of the server's stdout, and the write end
    /// of the server's stdin.  Performs the LSP initialize handshake.
    Result<void> connect(int read_fd, int write_fd);

    // ── Request / Response ───────────────────────────────────────────

    /// Send a JSON-RPC request and wait for the matching response.
    /// Blocks up to `timeout_sec` seconds.  Returns the full response
    /// message (which includes either `result` or `error`).
    /// If `cancelled` is provided, it is checked periodically during the wait
    /// (every ~200ms) and the request is cancelled if the token becomes true.
    Result<json> request(const std::string& method, json params,
                         int timeout_sec = 15,
                         std::shared_ptr<std::atomic<bool>> cancelled = nullptr);

    /// Send a JSON-RPC notification (fire-and-forget).
    Result<void> notify(const std::string& method, json params);

    /// Cancel an outstanding request by sending $/cancelRequest.
    void cancel_request(int id);

    // ── Lifecycle ────────────────────────────────────────────────────

    /// Ensure the server process is running.  If it has crashed, attempt
    /// an automatic restart using the same binary, args, and project root
    /// from the last `start()` call.  Does nothing if the server is already
    /// running or if start() was never called.
    /// Returns an error on restart failure (or if start() was never called).
    Result<void> ensure_running();

    /// Graceful shutdown: send shutdown request, then exit notification,
    /// then wait for the child to exit.
    Result<void> shutdown();

    /// True if the server process is believed to be running.
    bool is_running() const;

    // ── File synchronization ─────────────────────────────────────────

    /// Send textDocument/didOpen to register a file with the server.
    /// If the file is already open with the same content, this is a no-op.
    /// If the file is already open with different content, sends didChange.
    Result<void> open_file(const std::string& uri,
                           const std::string& language_id,
                           const std::string& content);

    /// Send textDocument/didChange with full content sync.
    Result<void> change_file(const std::string& uri,
                             const std::string& content, int version);

    /// Send textDocument/didClose.
    Result<void> close_file(const std::string& uri);

    /// True if the given URI is currently open in the LSP server.
    bool is_file_open(const std::string& uri) const;

    /// Ensure the server's view of a file matches the given content.
    /// If the file is not open, opens it.  If the content differs from
    /// what was last sent, sends didChange.
    Result<void> ensure_file_synced(const std::string& uri,
                                    const std::string& language_id,
                                    const std::string& content);

    /// Detect language ID from a file extension.
    static std::string language_id_from_extension(const std::string& path);

    // ── Accessors ────────────────────────────────────────────────────

    /// The server capabilities returned from the initialize response.
    const json& server_capabilities() const { return capabilities_; }

    /// Register a callback for server push notifications.
    using NotificationCallback = std::function<void(const json&)>;
    void on_notification(NotificationCallback cb) { on_notification_ = std::move(cb); }

    /// Set a cancellation token that will be checked during request() waits.
    /// If the token becomes true, pending requests will be cancelled with $/cancelRequest.
    void set_cancelled(std::shared_ptr<std::atomic<bool>> token) { cancelled_ = std::move(token); }

private:
    // Internal initialization shared by start() and connect().
    Result<void> do_handshake();
    void start_reader_thread();

    // Background thread: reads from the pipe and dispatches.
    void reader_thread_main();

    // I/O helpers
    bool write_raw(const std::string& data);
    std::optional<std::string> read_raw(int timeout_ms);

    // Stored start parameters (for crash recovery restart)
    std::string start_binary_path_;
    std::vector<std::string> start_args_;
    std::string start_project_root_;

    // Pipe fds (child's stdin/stdout from our perspective)
    int write_fd_ = -1;  // write → child's stdin
    int read_fd_ = -1;   // read  ← child's stdout

    // Child process
    pid_t child_pid_ = -1;

    // State
    std::atomic<bool> running_{false};
    json capabilities_;

    // Request tracking
    std::atomic<int> next_request_id_{1};
    std::mutex pending_mutex_;
    std::map<int, std::promise<json>> pending_;

    // Background thread
    std::thread reader_thread_;
    std::atomic<bool> reader_stop_{false};

    // Notification callback
    NotificationCallback on_notification_;

    // File state tracking
    struct FileState {
        bool is_open = false;
        int version = 0;
        size_t content_hash = 0;
        std::string content; // last-sent content (for re-sync after reconnect)
    };
    mutable std::mutex file_mutex_;
    std::map<std::string, FileState> files_;

    // Cancellation token (checked during request() waiting loops)
    std::shared_ptr<std::atomic<bool>> cancelled_;

    // Write mutex (serialize writes to the pipe)
    std::mutex write_mutex_;

    /// Compute a content hash for change detection.
    static size_t content_hash(const std::string& content);
};
