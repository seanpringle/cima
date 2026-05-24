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

Tool make_run_bwrap_tool(const Config& config, std::shared_ptr<std::string> safe_dir_ptr,
    int timeout, CancellationToken cancelled,
    bool read_only, bool allow_network) {
    Tool t;
    const std::string& safe_dir = *safe_dir_ptr;
    if (read_only) {
        t.name = "run_bwrap_ro";
        t.description = "Read-only: run a bash command in a bwrap sandbox (restricted filesystem, no network, no writes). Default cwd: " + safe_dir;
    } else if (allow_network) {
        t.name = "run_bwrap";
        t.description = "Run a bash command in a bwrap sandbox (restricted filesystem, with network access). Default cwd: " + safe_dir;
    } else {
        t.name = "run_bwrap";
        t.description = "Run a bash command in a bwrap sandbox (restricted filesystem, no network). Default cwd: " + safe_dir;
    }
    t.timeout_sec = 0; // manages its own timeout internally
    t.parameters = {{"type", "object"},
        {"properties",
            {{"command", {{"type", "string"}, {"description", "Shell command to execute"}}},
             {"cwd", {{"type", "string"}, {"description", "Working directory for the command (default: " + safe_dir + ")"}}}}},
        {"required", {"command"}}};
    t.execute = [safe_dir_ptr, timeout, cancelled, read_only, allow_network](const json& args) -> Result<std::string> {
        auto command = args.value("command", std::string());
        if (command.empty()) {
            return std::unexpected(std::string("command is required"));
        }
        auto cwd = args.value("cwd", std::string());

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
            prctl(PR_SET_PDEATHSIG, SIGKILL);

            // Create new process group so we can kill the whole process tree.
            setpgid(0, 0);

            // Build the bwrap command
            const std::string& safe_dir = *safe_dir_ptr;
            // Resolve working directory: use user-provided cwd, or default to safe_dir.
            std::string work_dir = cwd.empty() ? safe_dir : cwd;

            // We have 15 arguments plus 2 for the final command (sh, -c) plus
            // the command string itself = 18 fixed args + safe_dir twice + command.
            // Dynamically allocate at least 30 entries.
            const int max_args = 64;
            const char* argv[max_args];
            int argc = 0;

            argv[argc++] = "bwrap";

            // Read-only system paths
            argv[argc++] = "--ro-bind";
            argv[argc++] = "/usr";
            argv[argc++] = "/usr";

            argv[argc++] = "--ro-bind";
            argv[argc++] = "/etc";
            argv[argc++] = "/etc";

            // FHS symlinks
            argv[argc++] = "--symlink";
            argv[argc++] = "usr/lib";
            argv[argc++] = "/lib";

            argv[argc++] = "--symlink";
            argv[argc++] = "usr/lib64";
            argv[argc++] = "/lib64";

            argv[argc++] = "--symlink";
            argv[argc++] = "usr/bin";
            argv[argc++] = "/bin";

            argv[argc++] = "--symlink";
            argv[argc++] = "usr/sbin";
            argv[argc++] = "/sbin";

            // Essential filesystems
            argv[argc++] = "--proc";
            argv[argc++] = "/proc";

            argv[argc++] = "--dev";
            argv[argc++] = "/dev";

            argv[argc++] = "--tmpfs";
            argv[argc++] = "/tmp";

            // Bind the safe directory (read-write or read-only)
            if (!safe_dir.empty()) {
                argv[argc++] = read_only ? "--ro-bind" : "--bind";
                argv[argc++] = safe_dir.c_str();
                argv[argc++] = safe_dir.c_str();

                argv[argc++] = "--chdir";
                argv[argc++] = work_dir.c_str();
            }

            // Namespace isolation
            if (allow_network) {
                // Unshare all namespaces except network, so commands can
                // access the network (e.g. CMake FetchContent).
                argv[argc++] = "--unshare-all";
                argv[argc++] = "--share-net";
            } else {
                argv[argc++] = "--unshare-all";
            }
            argv[argc++] = "--new-session";
            argv[argc++] = "--die-with-parent";

            // The command to run
            argv[argc++] = "sh";
            argv[argc++] = "-c";
            argv[argc++] = command.c_str();
            argv[argc++] = nullptr;

            // Safety check: if we overflowed the array, abort
            if (argc >= max_args) {
                static const char msg[] = "error: too many bwrap arguments\n";
                write(STDOUT_FILENO, msg, sizeof(msg) - 1);
                _exit(1);
            }

            execvp("bwrap", const_cast<char**>(argv));

            // If execvp returns, bwrap wasn't found
            static const char msg[] = "error: bwrap not found — install bubblewrap (apt install bubblewrap)\n";
            write(STDOUT_FILENO, msg, sizeof(msg) - 1);
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

        return output;
    };
    return t;
}
