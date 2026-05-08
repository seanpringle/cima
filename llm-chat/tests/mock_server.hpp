#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

// -------------------------------------------------------------------
// Minimal HTTP mock server for testing chat completions
// -------------------------------------------------------------------
// Handler returns the response body.  If streaming=true the Content-Type
// is set to text/event-stream; otherwise application/json with Content-Length.
// Binds to a random port (port 0).  Port is available via port() / base_url().
// -------------------------------------------------------------------

class MockServer {
public:
    using Handler =
        std::function<std::string(const std::string& request)>;

    MockServer(Handler handler, bool streaming = false)
        : handler_(std::move(handler)), streaming_(streaming) {
        server_fd_ = socket(AF_INET, SOCK_STREAM, 0);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;

        bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        listen(server_fd_, 5);

        socklen_t len = sizeof(addr);
        getsockname(server_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);

        running_ = true;
        thread_ = std::jthread([this] { run(); });
    }

    ~MockServer() {
        running_ = false;
        if (server_fd_ >= 0) {
            close(server_fd_);
            server_fd_ = -1;
        }
    }

    int port() const { return port_; }
    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

private:
    void run() {
        while (running_) {
            struct pollfd pfd = {server_fd_, POLLIN, 0};
            int ret = poll(&pfd, 1, 200);
            if (!running_) break;
            if (ret <= 0) continue;

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client =
                accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr),
                       &client_len);
            if (client < 0)
                continue;

            char buf[8192] = {};
            ssize_t n = read(client, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                std::string request(buf);
                std::string body = handler_(request);

                std::string response;
                if (streaming_) {
                    response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/event-stream\r\n"
                        "Cache-Control: no-cache\r\n"
                        "\r\n" +
                        body;
                } else {
                    response =
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: " +
                        std::to_string(body.size()) + "\r\n" + "\r\n" + body;
                }
                write(client, response.data(), response.size());
            }
            close(client);
        }
    }

    int server_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::jthread thread_;
    Handler handler_;
    bool streaming_ = false;
};
