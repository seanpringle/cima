#include "lsp/lsp_client.h"
#include "lsp/json_rpc.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

// ===================================================================
// Construction / Destruction
// ===================================================================

LspClient::LspClient() = default;

LspClient::~LspClient() {
    if (running_) {
        shutdown();
    }
    if (reader_thread_.joinable()) {
        reader_stop_ = true;
        reader_thread_.join();
    }
    if (read_fd_ >= 0) close(read_fd_);
    if (write_fd_ >= 0) close(write_fd_);
}

// ===================================================================
// Connection
// ===================================================================

Result<void> LspClient::start(const std::string& binary_path,
                               const std::vector<std::string>& args,
                               const std::string& project_root) {
    // Create pipes: parent writes to child's stdin
    int stdin_pipe[2];
    int stdout_pipe[2];
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return std::unexpected(std::string("pipe() failed"));
    }

    child_pid_ = fork();
    if (child_pid_ == -1) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return std::unexpected(std::string("fork() failed"));
    }

    if (child_pid_ == 0) {
        // ── Child ──
        close(stdin_pipe[1]);   // close write end of stdin pipe
        close(stdout_pipe[0]);  // close read end of stdout pipe

        dup2(stdin_pipe[0], STDIN_FILENO);
        close(stdin_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdout_pipe[1]);
        dup2(STDOUT_FILENO, STDERR_FILENO); // merge stderr into stdout

        // Change to project root
        if (!project_root.empty()) {
            chdir(project_root.c_str());
        }

        // Build argv: argv[0] = binary_path, then args, then nullptr
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(binary_path.c_str());
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(binary_path.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    }

    // ── Parent ──
    write_fd_ = stdin_pipe[1];   // write to child's stdin
    read_fd_ = stdout_pipe[0];   // read from child's stdout
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    return do_handshake();
}

Result<void> LspClient::connect(int read_fd, int write_fd) {
    // Tear down any previous connection first.
    // Important: check the reader thread, not running_ — the reader may
    // have set running_=false after detecting EOF, but the thread object
    // still needs to be joined before we can start a new one.
    reader_stop_ = true;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (read_fd_ >= 0) close(read_fd_);
    if (write_fd_ >= 0) close(write_fd_);
    running_ = false;

    read_fd_ = read_fd;
    write_fd_ = write_fd;
    return do_handshake();
}

// ===================================================================
// Handshake
// ===================================================================

Result<void> LspClient::do_handshake() {
    // Start the background reader thread first so it can process
    // the initialize response.
    running_ = true;
    start_reader_thread();

    // Send initialize request
    auto resp = request("initialize", {
        {"processId", static_cast<int>(getpid())},
        {"capabilities", json::object()}
    }, 30);

    if (!resp) {
        running_ = false;
        return std::unexpected(resp.error());
    }

    // Extract capabilities from result
    auto& result = (*resp)["result"];
    if (result.contains("capabilities")) {
        capabilities_ = result["capabilities"];
    }

    // Send initialized notification
    auto notif_result = notify("initialized", {});
    if (!notif_result) {
        running_ = false;
        return std::unexpected(notif_result.error());
    }

    // Re-open any files that were being tracked from a previous session
    // (important after crash recovery reconnects).
    std::unique_lock<std::mutex> lock(file_mutex_);
    // Snapshot the tracked file states so we can send didOpen for each.
    std::vector<std::pair<std::string, FileState>> tracked;
    for (const auto& [uri, state] : files_) {
        if (state.is_open)
            tracked.emplace_back(uri, state);
    }
    // Clear the old state; open_file/change_file will repopulate.
    files_.clear();
    lock.unlock();

    for (const auto& [uri, state] : tracked) {
        // Re-send didOpen with the stored content so the new server
        // knows about this file.  If content is empty (older state),
        // just mark it open and let ensure_file_synced fill it in.
        if (!state.content.empty()) {
            auto result = open_file(uri, language_id_from_extension(uri),
                                     state.content);
            if (!result) {
                // Non-fatal — log and continue
                std::cerr << "LspClient: failed to re-open " << uri
                          << ": " << result.error() << std::endl;
            }
        } else {
            std::lock_guard<std::mutex> lock2(file_mutex_);
            files_[uri] = FileState{
                .is_open = true,
                .version = 1,
                .content_hash = 0,
            };
        }
    }

    return {};
}

// ===================================================================
// Background reader thread
// ===================================================================

void LspClient::start_reader_thread() {
    reader_stop_ = false;
    reader_thread_ = std::thread(&LspClient::reader_thread_main, this);
}

void LspClient::reader_thread_main() {
    std::string buf;
    std::vector<char> read_buf(65536);

    while (!reader_stop_ && running_) {
        // Use poll for non-blocking read with timeout so we can check
        // reader_stop_ periodically
        struct pollfd pfd;
        pfd.fd = read_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 200); // 200ms interval to check stop flag
        if (ret < 0) {
            if (errno == EINTR) continue;
            break; // error
        }
        if (ret == 0) continue; // timeout, loop back to check stop

        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & (POLLHUP | POLLERR)) {
                // Pipe closed (server exited)
                break;
            }
            continue;
        }

        ssize_t n = read(read_fd_, read_buf.data(), read_buf.size() - 1);
        if (n <= 0) {
            // EOF or error — server has exited
            break;
        }

        buf.append(read_buf.data(), n);

        // Process all complete messages in the buffer
        while (true) {
            auto decoded = lsp::decode_message(buf);
            if (!decoded)
                break; // need more data

            buf.erase(0, decoded->consumed);
            const json& msg = decoded->message;

            if (lsp::is_response(msg)) {
                int id = msg["id"].get<int>();
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                    it->second.set_value(msg);
                    pending_.erase(it);
                }
            } else if (lsp::is_notification(msg)) {
                if (on_notification_) {
                    on_notification_(msg);
                }
            }
            // Requests from the server are unexpected in this direction;
            // we ignore them.
        }
    }

    // Server has gone away — fulfill all pending requests with an error
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& [id, promise] : pending_) {
        try {
            promise.set_value(json::object()); // dummy value; caller should check running_
        } catch (...) {
            // promise already satisfied
        }
    }
    pending_.clear();
    running_ = false;
}

// ===================================================================
// Request / Response
// ===================================================================

Result<json> LspClient::request(const std::string& method, json params,
                                 int timeout_sec) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    int id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    json req = lsp::make_request_raw(id, method, std::move(params));

    // Create a promise/future pair for this request
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = std::move(*promise);
    }

    // Write the request
    auto wire = lsp::encode_message(req);
    if (!write_raw(wire)) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(id);
        return std::unexpected(std::string("failed to write to LSP server"));
    }

    // Wait for response with timeout
    auto status = future.wait_for(std::chrono::seconds(timeout_sec));
    if (status != std::future_status::ready) {
        // Timeout — cancel the request and clean up
        cancel_request(id);
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(id);
        return std::unexpected(
            std::string("LSP request timed out after ") +
            std::to_string(timeout_sec) + "s (method: " + method + ")");
    }

    json response = future.get();
    if (!running_) {
        return std::unexpected(
            std::string("LSP server connection closed while waiting for response"));
    }

    return response;
}

Result<void> LspClient::notify(const std::string& method, json params) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    json msg = lsp::make_notification(method, std::move(params));
    auto wire = lsp::encode_message(msg);
    if (!write_raw(wire)) {
        return std::unexpected(std::string("failed to send notification"));
    }
    return {};
}

void LspClient::cancel_request(int id) {
    // Send $/cancelRequest notification
    json cancel = lsp::make_notification("$/cancelRequest", {{"id", id}});
    auto wire = lsp::encode_message(cancel);
    write_raw(wire); // best-effort
}

// ===================================================================
// Shutdown
// ===================================================================

Result<void> LspClient::shutdown() {
    if (!running_ && child_pid_ <= 0)
        return {};

    running_ = false;

    if (child_pid_ > 0) {
        // Send shutdown request
        auto resp = request("shutdown", {}, 5);
        // Ignore response — server may already be gone

        // Send exit notification
        notify("exit", {});

        // Wait for child to exit
        int status;
        waitpid(child_pid_, &status, WNOHANG);
        // If it hasn't exited yet, give it a moment then SIGKILL
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t ret = waitpid(child_pid_, &status, WNOHANG);
            if (ret == child_pid_)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Force kill if still alive
        if (kill(child_pid_, 0) == 0) {
            kill(child_pid_, SIGKILL);
            waitpid(child_pid_, &status, 0);
        }
        child_pid_ = -1;
    }

    // Stop reader thread
    reader_stop_ = true;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    // Clean up pipe fds
    if (read_fd_ >= 0) {
        close(read_fd_);
        read_fd_ = -1;
    }
    if (write_fd_ >= 0) {
        close(write_fd_);
        write_fd_ = -1;
    }

    return {};
}

// ===================================================================
// File synchronization
// ===================================================================

Result<void> LspClient::open_file(const std::string& uri,
                                   const std::string& language_id,
                                   const std::string& content) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    auto hash = content_hash(content);

    std::unique_lock<std::mutex> lock(file_mutex_);
    auto it = files_.find(uri);

    if (it != files_.end() && it->second.is_open) {
        // Already open — check if content changed
        if (it->second.content_hash == hash) {
            // Same content, no-op
            return {};
        }
        // Content differs — upgrade to didChange
        int new_version = it->second.version + 1;
        lock.unlock();

        return change_file(uri, content, new_version);
    }
    lock.unlock();

    // New file — send didOpen
    json params = {
        {"textDocument", {
            {"uri", uri},
            {"languageId", language_id},
            {"version", 1},
            {"text", content},
        }}
    };

    auto result = notify("textDocument/didOpen", std::move(params));
    if (!result) return result;

    // Track state
    std::lock_guard<std::mutex> lock2(file_mutex_);
    files_[uri] = FileState{
        .is_open = true,
        .version = 1,
        .content_hash = hash,
        .content = content,
    };
    return {};
}

Result<void> LspClient::change_file(const std::string& uri,
                                     const std::string& content,
                                     int version) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    // Check that file is open (or was open before)
    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        auto it = files_.find(uri);
        if (it == files_.end() || !it->second.is_open) {
            return std::unexpected(
                std::string("file is not open: ") + uri +
                " — call open_file() first");
        }
    }

    json params = {
        {"textDocument", {
            {"uri", uri},
            {"version", version},
        }},
        {"contentChanges", json::array({
            {{"text", content}},
        })},
    };

    auto result = notify("textDocument/didChange", std::move(params));
    if (!result) return result;

    // Update state
    auto hash = content_hash(content);
    std::lock_guard<std::mutex> lock(file_mutex_);
    files_[uri] = FileState{
        .is_open = true,
        .version = version,
        .content_hash = hash,
        .content = content,
    };
    return {};
}

Result<void> LspClient::close_file(const std::string& uri) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    {
        std::lock_guard<std::mutex> lock(file_mutex_);
        auto it = files_.find(uri);
        if (it == files_.end() || !it->second.is_open) {
            return std::unexpected(
                std::string("file is not open: ") + uri);
        }
    }

    json params = {
        {"textDocument", {{"uri", uri}}}
    };

    auto result = notify("textDocument/didClose", std::move(params));
    if (!result) return result;

    std::lock_guard<std::mutex> lock(file_mutex_);
    files_.erase(uri);
    return {};
}

bool LspClient::is_file_open(const std::string& uri) const {
    std::lock_guard<std::mutex> lock(file_mutex_);
    auto it = files_.find(uri);
    return it != files_.end() && it->second.is_open;
}

Result<void> LspClient::ensure_file_synced(const std::string& uri,
                                            const std::string& language_id,
                                            const std::string& content) {
    if (!running_) {
        return std::unexpected(std::string("LSP server is not running"));
    }

    auto hash = content_hash(content);

    std::unique_lock<std::mutex> lock(file_mutex_);
    auto it = files_.find(uri);

    if (it == files_.end() || !it->second.is_open) {
        // File not open — open it
        lock.unlock();
        return open_file(uri, language_id, content);
    }

    // File is open — check if content is stale
    if (it->second.content_hash == hash) {
        return {}; // already in sync
    }

    int new_version = it->second.version + 1;
    lock.unlock();
    return change_file(uri, content, new_version);
}

// ===================================================================
// Helpers
// ===================================================================

size_t LspClient::content_hash(const std::string& content) {
    // Simple FNV-1a hash for content change detection.
    // This is not cryptographic — just for equality checking.
    size_t h = 14695981039346656037ULL;
    for (unsigned char c : content) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string LspClient::language_id_from_extension(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return "plaintext";
    auto ext = path.substr(dot);
    if (ext == ".c" || ext == ".h")          return "c";
    if (ext == ".cc" || ext == ".cpp" ||
        ext == ".cxx" || ext == ".h" ||
        ext == ".hpp" || ext == ".hxx" ||
        ext == ".c++" || ext == ".h++")      return "cpp";
    if (ext == ".py")                        return "python";
    if (ext == ".rs")                        return "rust";
    if (ext == ".go")                        return "go";
    if (ext == ".java")                      return "java";
    if (ext == ".ts" || ext == ".tsx")       return "typescript";
    if (ext == ".js" || ext == ".jsx")       return "javascript";
    if (ext == ".json")                      return "json";
    if (ext == ".md")                        return "markdown";
    if (ext == ".html" || ext == ".htm")     return "html";
    if (ext == ".css")                       return "css";
    if (ext == ".sh")                        return "shell";
    if (ext == ".yaml" || ext == ".yml")     return "yaml";
    if (ext == ".xml")                       return "xml";
    return "plaintext";
}


// ===================================================================
// I/O helpers
// ===================================================================

bool LspClient::write_raw(const std::string& data) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (write_fd_ < 0)
        return false;

    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = write(write_fd_, ptr, remaining);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        ptr += n;
        remaining -= n;
    }
    return true;
}

std::optional<std::string> LspClient::read_raw(int timeout_ms) {
    if (read_fd_ < 0)
        return std::nullopt;

    struct pollfd pfd;
    pfd.fd = read_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return std::nullopt;

    char buf[65536];
    ssize_t n = read(read_fd_, buf, sizeof(buf) - 1);
    if (n <= 0)
        return std::nullopt;

    return std::string(buf, n);
}

bool LspClient::is_running() const {
    return running_;
}
