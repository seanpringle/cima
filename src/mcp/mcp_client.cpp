#include "mcp/mcp_client.h"

#include <curl/curl.h>
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

McpClient::McpClient() = default;

McpClient::~McpClient() {
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

Result<void> McpClient::start_stdio(const std::string& command,
                                     const std::vector<std::string>& args,
                                     const std::string& cwd,
                                     const std::map<std::string, std::string>& env,
                                     int timeout_sec) {
    // Store parameters for potential crash recovery.
    start_command_ = command;
    start_args_ = args;
    start_cwd_ = cwd;
    start_env_ = env;
    start_timeout_sec_ = timeout_sec;

    // Create pipes: parent writes to child's stdin, reads from child's stdout.
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

        // Change directory if specified.
        if (!cwd.empty()) {
            chdir(cwd.c_str());
        }

        // Set environment variables if specified.
        if (!env.empty()) {
            for (const auto& [key, value] : env) {
                setenv(key.c_str(), value.c_str(), 1);
            }
        }

        // Build argv: argv[0] = command, then args, then nullptr.
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(command.c_str());
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(command.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    }

    // ── Parent ──
    write_fd_ = stdin_pipe[1];   // write to child's stdin
    read_fd_ = stdout_pipe[0];   // read from child's stdout
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    // Perform initialize handshake.
    return initialize();
}

Result<void> McpClient::start_http(const std::string& url,
                                    const std::string& api_key,
                                    int timeout_sec) {
    http_url_ = url;
    http_api_key_ = api_key;
    start_timeout_sec_ = timeout_sec;
    http_mode_ = true;
    running_ = true;

    // Perform initialize handshake immediately.
    return initialize();
}

Result<void> McpClient::connect(int read_fd, int write_fd) {
    // Tear down any previous connection first.
    reader_stop_ = true;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (read_fd_ >= 0) close(read_fd_);
    if (write_fd_ >= 0) close(write_fd_);
    running_ = false;

    read_fd_ = read_fd;
    write_fd_ = write_fd;

    return initialize();
}

// ===================================================================
// Initialize handshake
// ===================================================================

Result<void> McpClient::initialize() {
    if (http_mode_) {
        // ── HTTP transport: POST the initialize request ──
        json init_params = {
            {"protocolVersion", "2025-11-25"},
            {"capabilities", json::object()},
            {"clientInfo", {
                {"name", "cima"},
                {"version", "1.0"}
            }}
        };

        auto resp = http_request("initialize", std::move(init_params), start_timeout_sec_);
        if (!resp) {
            running_ = false;
            return std::unexpected(resp.error());
        }

        // Extract capabilities and serverInfo from result.
        auto& result = *resp;
        if (result.contains("capabilities")) {
            capabilities_ = result["capabilities"];
        }
        if (result.contains("serverInfo")) {
            server_info_ = result["serverInfo"];
        }

        // Send initialized notification as a separate POST (best-effort).
        json notif;
        notif["jsonrpc"] = "2.0";
        notif["method"] = "notifications/initialized";
        notif["params"] = json::object();
        http_request("notifications/initialized", json::object(), 5);

        return {};
    }

    // ── Stdio transport: start reader thread, send initialize, get response ──
    running_ = true;
    start_reader_thread();

    json init_params = {
        {"protocolVersion", "2025-11-25"},
        {"capabilities", json::object()},
        {"clientInfo", {
            {"name", "cima"},
            {"version", "1.0"}
        }}
    };

    auto resp = send_request("initialize", std::move(init_params), start_timeout_sec_);
    if (!resp) {
        running_ = false;
        return std::unexpected(resp.error());
    }

    // Extract capabilities and serverInfo from result.
    auto& result = *resp;
    if (result.contains("capabilities")) {
        capabilities_ = result["capabilities"];
    }
    if (result.contains("serverInfo")) {
        server_info_ = result["serverInfo"];
    }

    // Send initialized notification.
    json notif;
    notif["jsonrpc"] = "2.0";
    notif["method"] = "notifications/initialized";
    notif["params"] = json::object();
    if (!write_line(mcp_encode(notif))) {
        running_ = false;
        return std::unexpected(std::string("failed to send initialized notification"));
    }

    return {};
}

// ===================================================================
// MCP methods
// ===================================================================

Result<std::vector<Tool>> McpClient::list_tools() {
    auto resp = send_request("tools/list", json::object(), start_timeout_sec_);
    if (!resp) {
        return std::unexpected(resp.error());
    }

    std::vector<Tool> tools;
    auto& result = *resp;
    if (result.contains("tools") && result["tools"].is_array()) {
        for (const auto& t : result["tools"]) {
            Tool tool;
            tool.name = t.value("name", std::string());
            tool.description = t.value("description", std::string());
            if (t.contains("inputSchema") && t["inputSchema"].is_object()) {
                tool.parameters = t["inputSchema"];
                if (!tool.parameters.contains("type") || !tool.parameters["type"].is_string()) {
                    tool.parameters["type"] = "object";
                }
            } else {
                tool.parameters = json::object({
                    {"type", "object"},
                    {"properties", json::object()},
                    {"required", json::array()}
                });
            }
            tools.push_back(std::move(tool));
        }
    }

    return tools;
}

Result<std::string> McpClient::call_tool(const std::string& name, const json& arguments) {
    json params;
    params["name"] = name;
    params["arguments"] = arguments;

    auto resp = send_request("tools/call", std::move(params), start_timeout_sec_);
    if (!resp) {
        return std::unexpected(resp.error());
    }

    auto& result = *resp;

    // Check for MCP-level error (isError field in the result).
    if (result.contains("isError") && result["isError"].get<bool>()) {
        std::string err = result.value("content", json::array()).dump();
        return std::unexpected(std::string("MCP tool error: ") + err);
    }

    // Extract text content from the response.
    // MCP tool result format: {"content": [{"type": "text", "text": "..."}, ...]}
    std::string text;
    if (result.contains("content") && result["content"].is_array()) {
        for (const auto& item : result["content"]) {
            if (item.value("type", std::string()) == "text") {
                text += item.value("text", std::string());
            }
        }
    }

    return text;
}

// ===================================================================
// Shutdown
// ===================================================================

Result<void> McpClient::shutdown() {
    if (!running_ && child_pid_ <= 0)
        return {};

    running_ = false;

    if (http_mode_) {
        // ── HTTP transport: send shutdown + exit via POST (best-effort) ──
        http_request("shutdown", json::object(), 5);
        json exit_notif;
        exit_notif["jsonrpc"] = "2.0";
        exit_notif["method"] = "exit";
        exit_notif["params"] = json::object();
        http_request("exit", json::object(), 5);
        return {};
    }

    // ── Stdio transport: send shutdown/exit via pipes ──
    json shutdown_req;
    shutdown_req["jsonrpc"] = "2.0";
    shutdown_req["id"] = next_request_id_.fetch_add(1, std::memory_order_relaxed);
    shutdown_req["method"] = "shutdown";
    shutdown_req["params"] = json::object();
    write_line(mcp_encode(shutdown_req));

    // Give the server a moment to process shutdown, then send exit.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    json exit_notif;
    exit_notif["jsonrpc"] = "2.0";
    exit_notif["method"] = "exit";
    exit_notif["params"] = json::object();
    write_line(mcp_encode(exit_notif));

    // ── Wait for child to exit (if we have one) ──
    if (child_pid_ > 0) {
        int status;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            pid_t ret = waitpid(child_pid_, &status, WNOHANG);
            if (ret == child_pid_)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Force kill if still alive.
        if (kill(child_pid_, 0) == 0) {
            kill(child_pid_, SIGKILL);
            waitpid(child_pid_, &status, 0);
        }
        child_pid_ = -1;
    }

    // Stop reader thread.
    reader_stop_ = true;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }

    // Clean up pipe fds.
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
// Background reader thread
// ===================================================================

void McpClient::start_reader_thread() {
    reader_stop_ = false;
    reader_thread_ = std::thread(&McpClient::reader_thread_main, this);
}

void McpClient::reader_thread_main() {
    std::vector<char> read_buf(65536);

    while (!reader_stop_ && running_) {
        // Use poll for non-blocking read with timeout so we can check
        // reader_stop_ periodically.
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

        // Append to the line buffer.
        std::string buf;
        {
            std::lock_guard<std::mutex> lock(read_buf_mutex_);
            read_buf_.append(read_buf.data(), n);
            buf = read_buf_;
        }

        // Process all complete lines in the buffer.
        size_t consumed_total = 0;
        while (true) {
            auto decoded = mcp_decode(std::string_view(buf).substr(consumed_total));
            if (!decoded)
                break;

            consumed_total += decoded->consumed;
            const json& msg = decoded->message;

            // Skip empty/malformed messages.
            if (msg.is_null() || msg.empty())
                continue;

            if (msg.contains("id") && (msg.contains("result") || msg.contains("error"))) {
                // This is a response to a request.  Pass the full message
                // to send_request() so it can distinguish result vs error.
                int id = msg["id"].get<int>();

                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_.find(id);
                if (it != pending_.end()) {
                    it->second.set_value(msg);
                    pending_.erase(it);
                }
            } else if (msg.contains("method") && !msg.contains("id")) {
                // This is a notification.
                if (on_notification_) {
                    on_notification_(msg);
                }
            }
        }

        // Remove consumed bytes from the buffer.
        {
            std::lock_guard<std::mutex> lock(read_buf_mutex_);
            read_buf_.erase(0, consumed_total);
        }
    }

    // Server has gone away — fulfill all pending requests with an error.
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& [id, promise] : pending_) {
        try {
            promise.set_value(json()); // dummy value
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

// ===================================================================
// HTTP request (for streamable-http transport)
// ===================================================================

static size_t mcp_http_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t mcp_http_header_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(ptr, size * nmemb);
    // Parse "Key: Value" header lines
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 2);
        // Trim trailing \r\n
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();
        (*headers)[key] = value;
    }
    return size * nmemb;
}

Result<json> McpClient::http_request(const std::string& method, json params,
                                      int timeout_sec) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return std::unexpected(std::string("curl_easy_init failed"));
    }

    // Build the JSON-RPC request body.
    json request;
    request["jsonrpc"] = "2.0";

    // Notifications have no id.
    bool is_notification = (method == "notifications/initialized");
    if (!is_notification) {
        int id = next_request_id_.fetch_add(1, std::memory_order_relaxed);
        request["id"] = id;
    }

    request["method"] = method;
    request["params"] = params;
    std::string body = request.dump();

    std::string response_body;
    long http_code = 0;
    std::map<std::string, std::string> response_headers;

    curl_easy_setopt(curl, CURLOPT_URL, http_url_.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mcp_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, mcp_http_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima-mcp/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    // Build headers.
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "MCP-Protocol-Version: 2025-11-25");

    if (!http_api_key_.empty()) {
        std::string auth = "Authorization: Bearer " + http_api_key_;
        headers = curl_slist_append(headers, auth.c_str());
    }

    if (!session_id_.empty()) {
        std::string sid = "MCP-Session-Id: " + session_id_;
        headers = curl_slist_append(headers, sid.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return std::unexpected(
            std::string("MCP HTTP request failed: ") + curl_easy_strerror(res));
    }

    if (http_code < 200 || http_code >= 300) {
        return std::unexpected(
            std::string("MCP HTTP error: ") + std::to_string(http_code) +
            " " + response_body);
    }

    // Check for MCP-Session-Id in response headers.
    auto sid_it = response_headers.find("MCP-Session-Id");
    if (sid_it != response_headers.end() && !sid_it->second.empty()) {
        session_id_ = sid_it->second;
    }

    // For notifications, no response body is expected.
    if (is_notification) {
        return json::object(); // empty result
    }

    // Parse the JSON-RPC response.
    json response;
    try {
        response = json::parse(response_body);
    } catch (...) {
        return std::unexpected(
            std::string("MCP HTTP response is not valid JSON: ") + response_body);
    }

    // Check for JSON-RPC error.
    if (response.contains("error")) {
        auto& err = response["error"];
        std::string msg = err.value("message", "Unknown MCP error");
        return std::unexpected(std::string("MCP error: ") + msg);
    }

    // Return the result field.
    if (response.contains("result")) {
        return response["result"];
    }

    return std::unexpected(std::string("MCP response missing both result and error"));
}

// ===================================================================
// Request / Response (stdio transport)
// ===================================================================

Result<json> McpClient::send_request(const std::string& method, json params,
                                      int timeout_sec) {
    if (!running_) {
        return std::unexpected(std::string("MCP server is not running"));
    }

    // Route through HTTP if in HTTP mode.
    if (http_mode_) {
        return http_request(method, std::move(params), timeout_sec);
    }

    int id = next_request_id_.fetch_add(1, std::memory_order_relaxed);

    json request;
    request["jsonrpc"] = "2.0";
    request["id"] = id;
    request["method"] = method;
    request["params"] = std::move(params);

    // Create a promise/future pair for this request.
    auto promise = std::make_shared<std::promise<json>>();
    auto future = promise->get_future();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = std::move(*promise);
    }

    // Write the request as a newline-delimited JSON line.
    if (!write_line(mcp_encode(request))) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_.find(id);
        if (it != pending_.end()) {
            try { it->second.set_value(json()); } catch (...) {}
            pending_.erase(it);
        }
        return std::unexpected(std::string("failed to write to MCP server"));
    }

    // Wait for response with timeout, polling the cancellation token.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::seconds(timeout_sec);
    constexpr auto poll_interval = std::chrono::milliseconds(200);

    while (true) {
        // Check cancellation.
        if (cancelled_ && *cancelled_) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                try { it->second.set_value(json()); } catch (...) {}
                pending_.erase(it);
            }
            return std::unexpected(
                std::string("MCP request was cancelled (method: ") + method + ")");
        }

        // Check remaining time.
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_.find(id);
            if (it != pending_.end()) {
                try { it->second.set_value(json()); } catch (...) {}
                pending_.erase(it);
            }
            return std::unexpected(
                std::string("MCP request timed out after ") +
                std::to_string(timeout_sec) + "s (method: " + method + ")");
        }

        // Wait for the future with a short timeout so we can poll.
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        auto wait_time = std::min(poll_interval,
                                  std::chrono::milliseconds(std::max(remaining, 1L)));

        auto status = future.wait_for(wait_time);
        if (status == std::future_status::ready) {
            break;
        }
    }

    json response = future.get();
    if (!running_) {
        return std::unexpected(
            std::string("MCP server connection closed while waiting for response"));
    }

    // Check for JSON-RPC error response.
    if (response.contains("error")) {
        auto& err = response["error"];
        std::string msg = err.value("message", "Unknown MCP error");
        return std::unexpected(std::string("MCP error: ") + msg);
    }

    // Return the result field.
    if (response.contains("result")) {
        return response["result"];
    }

    return std::unexpected(std::string("MCP response missing both result and error"));
}

// ===================================================================
// Status
// ===================================================================

bool McpClient::is_running() const {
    return running_;
}

// ===================================================================
// I/O helpers — newline-delimited JSON
// ===================================================================

bool McpClient::write_line(const std::string& data) {
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

std::optional<std::string> McpClient::read_line(int timeout_ms) {
    if (read_fd_ < 0)
        return std::nullopt;

    // Check if we already have a complete line in the buffer.
    {
        std::lock_guard<std::mutex> lock(read_buf_mutex_);
        auto nl = read_buf_.find('\n');
        if (nl != std::string::npos) {
            std::string line = read_buf_.substr(0, nl);
            read_buf_.erase(0, nl + 1);
            return line;
        }
    }

    // Read more data until we have a complete line or timeout.
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    char tmp[4096];

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining < 0)
            return std::nullopt; // timeout

        struct pollfd pfd;
        pfd.fd = read_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, std::min(remaining, 5000L));
        if (ret <= 0)
            return std::nullopt; // timeout or error

        ssize_t n = read(read_fd_, tmp, sizeof(tmp));
        if (n <= 0)
            return std::nullopt; // EOF or error

        std::lock_guard<std::mutex> lock(read_buf_mutex_);
        read_buf_.append(tmp, n);

        auto nl = read_buf_.find('\n');
        if (nl != std::string::npos) {
            std::string line = read_buf_.substr(0, nl);
            read_buf_.erase(0, nl + 1);
            return line;
        }
    }
}
