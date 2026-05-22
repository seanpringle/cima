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

Tool make_run_bash_tool(std::shared_ptr<std::string> safe_dir_ptr,
    int timeout, CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "run_bash";
    t.description = "Run a bash command in the project directory "
                    "(e.g. build, test, lint). "
                    "Long output (>100 lines or 4K chars) is redirected to the tool log.";
    t.timeout_sec = 0; // bash manages its own timeout internally
    t.parameters = {{"type", "object"},
        {"properties",
            {{"command", {{"type", "string"}, {"description", "Shell command to execute"}}}}},
        {"required", {"command"}}};
    t.execute = [safe_dir_ptr, timeout, cancelled, tool_logs](const json& args) -> Result<std::string> {
        auto command = args.value("command", std::string());
        if (command.empty()) {
            return std::unexpected(std::string("command is required"));
        }

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

            // Redirect stdin from /dev/null so commands that read stdin
            // exit immediately instead of blocking forever.
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull != -1) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }

            // Ensure grandchildren are killed when this child dies
            // (covers the edge case where setpgid fails and killpg falls
            // back to kill() which only targets the direct child).
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // Create new process group so we can kill the whole process tree.
            // Both parent and child call setpgid to avoid a race (whichever
            // runs first succeeds; the other gets EACCES which we ignore).
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
        // Both parent and child call setpgid to avoid a race (whichever
        // runs first succeeds; the other gets EACCES which we ignore).
        setpgid(pid, pid);


        std::string output;
        char buf[4096];
        const int timeout_secs = timeout;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

        // Set read end to non-blocking
        int flags = fcntl(pipefd[0], F_GETFL, 0);
        if (flags != -1) {
            fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
        }

        auto kill_child = [&] {
            // killpg targets the process group; fall back to kill if the
            // child didn't successfully become a process group leader.
            if (killpg(pid, SIGKILL) != 0) {
                kill(pid, SIGKILL);
            }
            int st;
            waitpid(pid, &st, 0);
        };

        while (true) {
            if (cancelled && *cancelled) {
                kill_child();
                close(pipefd[0]);
                return output + "\n(interrupted)";
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                kill_child();
                close(pipefd[0]);
                return output + "\n(timed out)";
            }

            ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);

            if (n > 0) {
                buf[n] = '\0';
                output += buf;
            } else if (n == 0) {
                break; // EOF
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

        // Annotate failures so the caller can see them.
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code != 0) {
                output += "\n(exit code: " + std::to_string(code) + ")";
            }
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            output += "\n(killed by signal: " + std::to_string(sig) + ")";
        }

        return spill_long_output(std::move(output), tool_logs);
    };
    return t;
}
