#include "tools.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

Tool make_cmd_tool(const std::string& name,
    const std::string& description,
    const std::string& command,
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout,
    CancellationToken cancelled) {
    Tool t;
    // Use "cmd_<name>" as the tool name visible to the model
    t.name = "cmd_" + name;
    t.description = description;
    t.permission = ToolPermission::Write;
    t.timeout_sec = 0; // internal timeout via fork/exec loop
    // Empty parameters ensures the model sends {} not null
    t.parameters = {
        {"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [safe_dir_ptr, command, timeout, cancelled](const json& /*args*/) -> Result<std::string> {
        // --- fork + exec with pipe and timeout ---
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            return std::unexpected(std::string("pipe() failed"));
        }

        pid_t pid = fork();
        if (pid == -1) {
            close(pipefd[0]);
            close(pipefd[1]);
            return std::unexpected(std::string("fork() failed"));
        }

        if (pid == 0) {
            // ---- child ----
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            if (pipefd[1] > STDERR_FILENO)
                close(pipefd[1]);

            prctl(PR_SET_PDEATHSIG, SIGKILL);
            setpgid(0, 0);

            if (!safe_dir_ptr->empty()) {
                if (chdir(safe_dir_ptr->c_str()) != 0) {
                    static const char msg[] = "error: chdir() to safe directory failed\n";
                    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
                    _exit(1);
                }
            }

            execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
            _exit(127);
        }

        // ---- parent ----
        close(pipefd[1]);
        setpgid(pid, pid);

        std::string output;
        char buf[4096];
        const int timeout_secs = timeout;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags != -1) {
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        }

        auto kill_child = [&] {
            if (killpg(pid, SIGKILL) != 0) {
                kill(pid, SIGKILL);
            }
            int st;
            waitpid(pid, &st, 0);
        };

        auto truncate_output = [](std::string& out) {
            if (out.size() > 16000) {
                out = out.substr(0, 16000) + "...(truncated, >16000 chars)";
            }
        };

        while (true) {
            if (cancelled && *cancelled) {
                kill_child();
                close(pipefd[0]);
                truncate_output(output);
                return output + "\n(interrupted)";
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                kill_child();
                close(pipefd[0]);
                truncate_output(output);
                return output;
            }

            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);

            if (n > 0) {
                buf[n] = '\0';
                output += buf;
                if (output.size() > 16000) {
                    kill_child();
                    close(pipefd[0]);
                    output = output.substr(0, 16000) + "...(truncated, >16000 chars)";
                    return output;
                }
            } else if (n == 0) {
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {pipefd[0], POLLIN, 0};
                auto remaining =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
                if (remaining > 0) {
                    poll(&pfd, 1, std::min(remaining, 100L));
                }
            } else {
                break;
            }
        }

        close(pipefd[0]);
        int status;
        waitpid(pid, &status, 0);

        // Line count truncation
        int nl = 0;
        for (char c : output)
            if (c == '\n')
                nl++;
        if (nl > 500) {
            size_t pos = 0;
            for (int i = 0; i < 500; i++) {
                pos = output.find('\n', pos) + 1;
            }
            output = output.substr(0, pos) + "...(truncated, >500 lines)";
        }

        truncate_output(output);

        return output;
    };
    return t;
}
