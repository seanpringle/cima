#include "tools.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

// ── Trim leading/trailing whitespace (prevents trivial bypass of
//    exact-match checks like "-exec" → " -exec").
static std::string trim(std::string s) {
    auto ws = [](unsigned char c) { return std::isspace(c); };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), ws));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), ws).base(), s.end());
    return s;
}

// ===================================================================
// Shared helper: validate command + args
// ===================================================================

static Result<std::pair<std::string, std::vector<std::string>>>
validate_exec_args(const json& args,
    const std::set<std::string>& whitelist,
    std::shared_ptr<std::string> safe_dir_ptr) {
    auto cmd = trim(args.value("cmd", std::string()));
    if (cmd.empty()) {
        return std::unexpected(std::string("cmd is required"));
    }

    // Command must be a simple name (no path separators)
    if (cmd.find('/') != std::string::npos) {
        return std::unexpected(
            std::string("cmd must be a simple command name, not a path: ") + cmd);
    }

    // Must be in the whitelist
    if (whitelist.find(cmd) == whitelist.end()) {
        return std::unexpected(
            std::string("command '") + cmd + "' is not in the allowed commands list");
    }

    // Validate arguments that look like paths
    auto arg_array = args.value("args", json::array());
    std::vector<std::string> validated_args;

    for (const auto& arg : arg_array) {
        if (!arg.is_string()) {
            return std::unexpected("args must be strings");
        }
        std::string a = trim(arg.get<std::string>());
        if (a.empty()) {
            validated_args.push_back(a);
            continue;
        }

        // Determine if this arg "looks like a path"
        bool looks_like_path = false;

        // Check for path-like characteristics
        if (a[0] == '/' || a[0] == '.') {
            // Starts with / or . — definitely a path (absolute or relative reference)
            looks_like_path = true;
        } else {
            // Check for any path separators
            for (size_t i = 0; i < a.size(); i++) {
                if (a[i] == '/' || a[i] == '\\') {
                    looks_like_path = true;
                    break;
                }
            }
        }

        if (looks_like_path) {
            // Try to resolve the path.
            auto resolved = resolve_path(a, *safe_dir_ptr);
            if (resolved) {
                // The path resolved to something under safe_dir.
                // Now check if it's actually a filesystem path or just an argument
                // that happens to contain '/' (e.g. sed script "s/hello/goodbye/g").
                bool is_real_path = false;
                if (a[0] == '/' || a[0] == '.') {
                    // Starts with / or . — definitely a filesystem path
                    is_real_path = true;
                } else {
                    // Contains / — could be a path or a script arg.
                    // Check if the resolved path (or its parent) exists on disk.
                    // Also check if the part before the first / looks like a directory
                    // that exists within safe_dir.
                    std::error_code ec;
                    if (std::filesystem::exists(*resolved, ec)) {
                        is_real_path = true;
                    } else {
                        // Check if parent directory exists (for new-file paths like
                        // "subdir/newfile.txt" or "a/b/c/" for mkdir -p)
                        auto parent = std::filesystem::path(*resolved).parent_path();
                        if (std::filesystem::exists(parent, ec)) {
                            is_real_path = true;
                        }
                    }
                }
                if (is_real_path) {
                    validated_args.push_back(*resolved);
                } else {
                    // Not a real filesystem path — pass through unchanged
                    validated_args.push_back(a);
                }
            } else if (a[0] == '/' || a[0] == '.') {
                // Absolute or dot-relative paths MUST resolve — reject if they don't
                return std::unexpected(resolved.error());
            } else {
                // Contains / but doesn't resolve to a valid path under safe_dir.
                // This is likely a non-path argument that happens to contain /
                // (e.g. sed script "s/hello/goodbye/g"). Pass it through unchanged.
                validated_args.push_back(a);
            }
        } else {
            // Not a path, pass through unchanged (e.g. flags like -l, -n 5)
            validated_args.push_back(a);
        }
    }

    // ── cmd-specific safety checks ──
    if (cmd == "find") {
        // find's -exec/-execdir/-ok predicates execute arbitrary shell commands,
        // and -delete modifies the filesystem — none of these belong in a
        // read-only tool (or even a read-write one, since the command lives
        // entirely inside the arg string, not in a validated path).
        for (const auto& a : validated_args) {
            if (a == "-exec" || a == "-execdir" || a == "-ok" || a == "-delete") {
                return std::unexpected(
                    "find with '" + a + "' is not allowed (shell-execution vector)");
            }
        }
    } else if (cmd == "sort") {
        // sort's -o/--output flag writes to a file, which is inappropriate for
        // a read-only tool.  (If in-place sorting is needed, use exec_rw or run_bash.)
        for (size_t i = 0; i < validated_args.size(); i++) {
            const auto& a = validated_args[i];
            if (a == "-o" || a == "--output") {
                // The next arg is the output file — reject
                return std::unexpected(
                    "sort with -o/--output is not allowed in exec_ro (writes to a file)");
            }
            if (a.starts_with("-o") && a.size() > 2) {
                // Combined form: -oFILE
                return std::unexpected(
                    "sort with -o/--output is not allowed in exec_ro (writes to a file)");
            }
            if (a.starts_with("--output=")) {
                return std::unexpected(
                    "sort with -o/--output is not allowed in exec_ro (writes to a file)");
            }
        }
    } else if (cmd == "patch") {
        // -p0 makes patch resolve filenames in the diff with no component
        // stripping, which can traverse upward via relative paths in diff
        // headers.  --posix enables POSIX mode, which allows absolute paths
        // in diff headers (default since 2.7 is to reject them).
        for (size_t i = 0; i < validated_args.size(); i++) {
            if (validated_args[i] == "-p0") {
                return std::unexpected(
                    "patch with -p0 is not allowed (path-traversal vector)");
            }
            if (validated_args[i] == "-p" && i + 1 < validated_args.size() &&
                validated_args[i + 1] == "0") {
                return std::unexpected(
                    "patch with -p0 is not allowed (path-traversal vector)");
            }
            if (validated_args[i] == "--posix") {
                return std::unexpected(
                    "patch with --posix is not allowed (enables absolute paths from diff headers)");
            }
        }
    }

    return std::make_pair(cmd, validated_args);
}

// ===================================================================
// Shared helper: fork + execvp with timeout and output collection
// ===================================================================

static Result<std::string> run_exec(
    const std::string& cmd,
    const std::vector<std::string>& args,
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout_secs,
    CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {

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

        // Build argv array: [cmd, arg1, arg2, ..., nullptr]
        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // ---- parent ----
    close(pipefd[1]);
    setpgid(pid, pid);

    std::string output;
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

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

    // Check exit status and annotate failures so the caller can see them.
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            output += "\n(exit code: " + std::to_string(code) + ")";
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        output += "\n(killed by signal: " + std::to_string(sig) + ")";
    }

    // Spill to tool_logs if output exceeds threshold
    if (tool_logs) {
        int nl = 0;
        for (char c : output)
            if (c == '\n') nl++;
        if (nl > 100 || output.size() > 4096) {
            size_t id = tool_logs->size() + 1;
            tool_logs->push_back(std::move(output));
            return "(long tool output: " + std::to_string(nl) + " lines, " +
                   std::to_string(tool_logs->back().size()) + " chars. "
                   "Use view_tool_output(id=" + std::to_string(id) + ") to read it)";
        }
    }

    return output;
}

// ── Build an "Allowed commands: ..." line from a whitelist set ──
static std::string allowed_commands_line(const std::set<std::string>& cmds) {
    std::string out = "Allowed commands: ";
    bool first = true;
    for (const auto& c : cmds) {
        if (!first) out += ", ";
        out += c;
        first = false;
    }
    out += ".";
    return out;
}

// ===================================================================
// make_exec_ro_tool
// ===================================================================

Tool make_exec_ro_tool(
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout,
    CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "exec_ro";
    t.description =
        std::string("Execute a single read-only command on a file or path. "
        "This is NOT a general-purpose shell — no pipes, no redirects, "
        "no sequences. Useful for inspecting files and paths. ") +
        allowed_commands_line(exec_ro_allowed_commands) + " "
        "Safety: find -exec/-execdir/-ok/-delete blocked; "
        "sort -o/--output blocked. "
        "Long output (>100 lines or 4K chars) is redirected to the tool log.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 0; // internal timeout via fork/exec loop
    t.parameters = {{"type", "object"},
        {"properties",
            {{"cmd",
                {{"type", "string"},
                    {"description",
                        "Command name (must be in the read-only whitelist)"}}},
             {"args",
                {{"type", "array"}, {"items", {{"type", "string"}}},
                    {"description", "Arguments to pass to the command"}}}}},
        {"required", {"cmd"}}};
    t.execute = [safe_dir_ptr, timeout, cancelled, tool_logs](const json& args) -> Result<std::string> {
        auto validated = validate_exec_args(args, exec_ro_allowed_commands, safe_dir_ptr);
        if (!validated) {
            return std::unexpected(validated.error());
        }
        auto& [cmd, cmd_args] = *validated;
        return run_exec(cmd, cmd_args, safe_dir_ptr, timeout, cancelled, tool_logs);
    };
    return t;
}

// ===================================================================
// make_exec_rw_tool
// ===================================================================

Tool make_exec_rw_tool(
    std::shared_ptr<std::string> safe_dir_ptr,
    int timeout,
    CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "exec_rw";
    t.description =
        std::string("Execute a single read-write command on a file or path. "
        "This is NOT a general-purpose shell — no pipes, no redirects, "
        "no sequences. Useful for modifying files and the filesystem. ") +
        allowed_commands_line(exec_rw_allowed_commands) + " "
        "Safety: sed is run with --sandbox (e/r/w disabled); "
        "patch -p0 and --posix are rejected. "
        "Long output (>100 lines or 4K chars) is redirected to the tool log.";
    t.permission = ToolPermission::Write;
    t.timeout_sec = 0; // internal timeout via fork/exec loop
    t.parameters = {{"type", "object"},
        {"properties",
            {{"cmd",
                {{"type", "string"},
                    {"description",
                        "Command name (must be in the read-write whitelist)"}}},
             {"args",
                {{"type", "array"}, {"items", {{"type", "string"}}},
                    {"description", "Arguments to pass to the command"}}}}},
        {"required", {"cmd"}}};
    t.execute = [safe_dir_ptr, timeout, cancelled, tool_logs](const json& args) -> Result<std::string> {
        auto validated = validate_exec_args(args, exec_rw_allowed_commands, safe_dir_ptr);
        if (!validated) {
            return std::unexpected(validated.error());
        }
        auto& [cmd, cmd_args] = *validated;
        // Force --sandbox on sed to disable e/r/w commands that could
        // execute arbitrary shell commands from within the script,
        // bypassing the path-argument sandbox.
        if (cmd == "sed") {
            cmd_args.insert(cmd_args.begin(), "--sandbox");
        }
        // The default behavior of GNU patch 2.7+ rejects absolute paths
        // in diff headers — we just need to make sure --posix isn't used
        // (which would re-enable them).
        return run_exec(cmd, cmd_args, safe_dir_ptr, timeout, cancelled, tool_logs);
    };
    return t;
}
