#include "tools.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

// ── Shared helper: run a shell command with timeout + cancellation ──
static Result<std::string> run_cmake_command(
    const std::string& command,
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout_sec,
    CancellationToken cancelled) {

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
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_sec);

    // Set read end to non-blocking
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
}

// ── Helper: build the command string with optional head/tail filtering ──
static std::string build_cmd(const std::string& base_cmd, int head, int tail) {
    std::string cmd = base_cmd + " 2>&1";
    if (head > 0) {
        cmd += " | head -n " + std::to_string(head);
    }
    if (tail > 0) {
        cmd += " | tail -n " + std::to_string(tail);
    }
    return cmd;
}

// ===================================================================
// cmake_configure
// ===================================================================

Tool make_cmake_configure_tool(std::shared_ptr<std::string> safe_dir_ptr,
    int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "cmake_configure";
    t.description =
        "Run `cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON` "
        "to configure the project into a build/ folder and enable "
        "generating compile commands.\n"
        "Returns raw combined stdout/stderr.\n"
        "If H is 0, omit head step. If T is 0, omit tail step.\n"
        "Use `flags` array for additional cmake -D flags, "
        "e.g. flags=[\"-DCMAKE_BUILD_TYPE=Debug\", \"-DBUILD_TESTS=OFF\"].";
    t.permission = ToolPermission::Write;
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"head",
                {{"type", "integer"},
                    {"description", "Take first N lines (0 = no head filter, default 0)"}}},
             {"tail",
                {{"type", "integer"},
                    {"description", "Take last N lines (0 = no tail filter, default 0)"}}},
             {"flags",
                {{"type", "array"}, {"items", {{"type", "string"}}},
                    {"description",
                        "Optional array of additional flags, e.g. "
                        "[\"-DCMAKE_BUILD_TYPE=Debug\"]"}}}}},
        {"required", json::array()}};
    t.execute = [safe_dir_ptr, timeout, cancelled](const json& args) -> Result<std::string> {
        int head = args.value("head", 0);
        int tail = args.value("tail", 0);
        std::string base = "cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON";

        auto flags = args.value("flags", json::array());
        if (!flags.is_null() && flags.is_array()) {
            for (const auto& f : flags) {
                if (!f.is_string())
                    continue;
                std::string s = f.get<std::string>();
                // Shell-escape: allow only flag-safe characters
                for (char c : s) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' &&
                        c != '.' && c != '/' && c != '=' && c != ':' && c != '+' && c != '%' &&
                        c != ',' && c != '@' && c != '$' && c != '(' && c != ')' && c != ' ' &&
                        c != '\\') {
                        return std::unexpected(
                            std::string("Invalid character in flag: '") + c +
                            "'. Only flag-safe characters are allowed.");
                    }
                }
                base += " " + s;
            }
        }

        std::string cmd = build_cmd(base, head, tail);
        return run_cmake_command(cmd, safe_dir_ptr, timeout, cancelled);
    };
    return t;
}

// ===================================================================
// cmake_build
// ===================================================================

Tool make_cmake_build_tool(std::shared_ptr<std::string> safe_dir_ptr,
    int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "cmake_build";
    t.description =
        "Run `cmake --build build/ -j$(nproc)` to build the project.\n"
        "Returns raw combined stdout/stderr.\n"
        "If H is 0, omit head step. If T is 0, omit tail step.\n"
        "Use `target` to build only a specific target, "
        "e.g. target=\"test_tools\".\n"
        "Use `clean` to clean before building, "
        "e.g. clean=true (implies rebuild).";
    t.permission = ToolPermission::Write;
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"head",
                {{"type", "integer"},
                    {"description", "Take first N lines (0 = no head filter, default 0)"}}},
             {"tail",
                {{"type", "integer"},
                    {"description", "Take last N lines (0 = no tail filter, default 0)"}}},
             {"target",
                {{"type", "string"},
                    {"description",
                        "Optional CMake target to build, e.g. \"test_tools\", "
                        "\"cima\", \"cima_core\". Empty builds all."}}},
             {"clean",
                {{"type", "boolean"},
                    {"description",
                        "If true, clean the target first (runs cmake --build "
                        "--target clean before building)."}}}}},
        {"required", json::array()}};
    t.execute = [safe_dir_ptr, timeout, cancelled](const json& args) -> Result<std::string> {
        int head = args.value("head", 0);
        int tail = args.value("tail", 0);
        bool clean = args.value("clean", false);
        std::string target = args.value("target", std::string());

        // Shell-escape target name
        if (!target.empty()) {
            for (char c : target) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' &&
                    c != '.' && c != '/' && c != '+' && c != ':') {
                    return std::unexpected(
                        std::string("Invalid character in target name: '") + c +
                        "'. Only alphanumeric and -_. characters are allowed.");
                }
            }
        }

        std::string cmake_cmd = "cmake --build build/ -j$(nproc)";
        if (!target.empty()) {
            cmake_cmd += " --target " + target;
        }

        std::string full_cmd;
        if (clean) {
            full_cmd = "cmake --build build/ --target clean";
            full_cmd += " && " + cmake_cmd;
        } else {
            full_cmd = cmake_cmd;
        }

        std::string cmd = build_cmd(full_cmd, head, tail);
        return run_cmake_command(cmd, safe_dir_ptr, timeout, cancelled);
    };
    return t;
}

// ===================================================================
// cmake_ctest
// ===================================================================

Tool make_cmake_ctest_tool(std::shared_ptr<std::string> safe_dir_ptr,
    int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "cmake_ctest";
    t.description =
        "Run `ctest --test-dir build --output-on-failure -j$(nproc)` "
        "to run the test suite.\n"
        "Returns raw combined stdout/stderr.\n"
        "If H is 0, omit head step. If T is 0, omit tail step.\n"
        "Use `test_regex` to filter which tests run. "
        "This is passed directly to `ctest -R` as a regex "
        "(see `man ctest` for regex rules).\n"
        "Test names are the Catch2 TEST_CASE descriptions "
        "(free-form text like `\"Config defaults\"` or "
        "`\"SSEParser single complete event\"`), "
        "not source filenames.\n"
        "Use `|` for multiple patterns, "
        "e.g. test_regex=\"Config|SSEParser\" "
        "runs all Config and SSEParser tests.\n"
        "Example: test_regex=\"Config\" runs all config tests.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"head",
                {{"type", "integer"},
                    {"description", "Take first N lines (0 = no head filter, default 0)"}}},
             {"tail",
                {{"type", "integer"},
                    {"description", "Take last N lines (0 = no tail filter, default 0)"}}},
             {"test_regex",
                {{"type", "string"},
                    {"description",
                        "Optional regex pattern to filter which tests run "
                        "(passed to `ctest -R`). "
                        "Use `|` for multiple patterns, "
                        "e.g. \"Config|LspClient\". "
                        "Test names are Catch2 TEST_CASE descriptions "
                        "(free-form text like \"Config defaults\"), "
                        "not filenames."}}}}},
        {"required", json::array()}};
    t.execute = [safe_dir_ptr, timeout, cancelled](const json& args) -> Result<std::string> {
        int head = args.value("head", 0);
        int tail = args.value("tail", 0);
        std::string base = "ctest --test-dir build --output-on-failure -j$(nproc)";
        auto test_regex = args.value("test_regex", std::string());
        if (!test_regex.empty()) {
            // Shell-escape: reject anything that could break out of the -R argument.
            // Allow regex-safe characters: word chars, common regex metacharacters.
            for (char c : test_regex) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' &&
                    c != '.' && c != ',' && c != ' ' && c != '*' && c != '?' && c != '/' &&
                    c != '|' && c != '^' && c != '$' && c != '(' && c != ')' && c != '[' &&
                    c != ']' && c != '\\') {
                    return std::unexpected(
                        std::string("Invalid character in test_regex filter: '") + c +
                        "'. Only regex-safe characters are allowed.");
                }
            }
            base += " -R \"" + test_regex + "\"";
        }
        std::string cmd = build_cmd(base, head, tail);
        return run_cmake_command(cmd, safe_dir_ptr, timeout, cancelled);
    };
    return t;
}
