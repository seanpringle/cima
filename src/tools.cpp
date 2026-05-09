#include "tools.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <poll.h>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

// ===================================================================
// Path sandbox
// ===================================================================

Result<std::string> resolve_path(const std::string& raw_path, const std::string& safe_dir) {
  if (raw_path.empty()) {
    return std::unexpected(std::string("path is required"));
  }

  std::error_code ec;
  std::filesystem::path p(raw_path);

  // For relative paths, resolve against safe_dir first, then normalize
  if (p.is_relative()) {
    p = std::filesystem::path(safe_dir) / p;
  }

  p = std::filesystem::weakly_canonical(p, ec);
  if (ec) {
    p = std::filesystem::path(raw_path).lexically_normal();
    if (p.is_relative()) {
      p = std::filesystem::path(safe_dir) / p;
    }
  }

  std::string resolved = p.string();

  // Normalize safe_dir (no trailing slash)
  auto sd_path = std::filesystem::weakly_canonical(std::filesystem::path(safe_dir), ec);
  if (ec) {
    sd_path = std::filesystem::path(safe_dir).lexically_normal();
  }
  std::string sd = sd_path.string();
  while (!sd.empty() && sd.back() == '/') {
    sd.pop_back();
  }

  if (resolved == sd || resolved.starts_with(sd + "/")) {
    return resolved;
  }

  return std::unexpected("path must be under " + sd);
}

// ===================================================================
// Tool helpers
// ===================================================================

static Tool make_list_files_tool(const std::string& safe_dir) {
  Tool t;
  t.name = "list_files";
  t.description = "List files and directories in a given path";
  t.parameters = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}, {"description", "Directory path to list"}}}}}, {"required", {"path"}}};
  t.execute = [safe_dir](const json& args) -> Result<std::string> {
    auto raw = args.value("path", std::string());
    auto resolved = resolve_path(raw, safe_dir);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }

    std::string result;
    for (const auto& entry : std::filesystem::directory_iterator(*resolved)) {
      char type = '-';
      if (entry.is_directory())
        type = 'd';
      else if (entry.is_symlink())
        type = 'l';
      result += type;
      result += ' ';
      result += entry.path().filename().string();
      result += '\n';
    }
    if (result.empty()) {
      result = "(empty directory)";
    }
    return result;
  };
  return t;
}

static Tool make_read_file_tool(const std::string& safe_dir) {
  Tool t;
  t.name = "read_file";
  t.description = "Read lines from a file (max 200 lines at a time, use offset to "
                  "paginate)";
  t.parameters = {{"type", "object"},
                  {"properties",
                   {{"path", {{"type", "string"}, {"description", "Path to the file to read"}}},
                    {"offset",
                     {{"type", "integer"},
                      {"description",
                       "Line number to start from (1-indexed, default 0 = "
                       "beginning)"}}},
                    {"max_lines", {{"type", "integer"}, {"description", "Maximum lines to read starting from offset (default 200)"}}}}},
                  {"required", {"path"}}};
  t.execute = [safe_dir](const json& args) -> Result<std::string> {
    auto raw = args.value("path", std::string());
    auto resolved = resolve_path(raw, safe_dir);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }

    int offset = args.value("offset", 0);
    int max_lines = args.value("max_lines", 200);
    if (offset < 0)
      offset = 0;
    if (max_lines < 1)
      max_lines = 1;

    std::ifstream file(*resolved);
    if (!file.is_open()) {
      return std::unexpected("Failed to open file: " + *resolved);
    }

    std::string result;
    std::string line;
    int line_num = 0;
    int count = 0;

    // Skip lines before offset
    while (line_num < offset && std::getline(file, line)) {
      line_num++;
    }

    while (std::getline(file, line) && count < max_lines) {
      line_num++;
      result += line;
      result += '\n';
      count++;
    }

    if (!file.eof()) {
      result += "...(truncated, >" + std::to_string(max_lines) + " lines from offset " + std::to_string(offset) + ")";
    } else if (count == 0 && offset > 0) {
      result = "(offset " + std::to_string(offset) + " is beyond end of file)";
    }
    return result;
  };
  return t;
}

static Tool make_grep_files_tool(const std::string& safe_dir) {
  Tool t;
  t.name = "grep_files";
  t.description = "Search file contents using a regex pattern (max 50 results)";
  t.timeout_sec = 10;
  t.parameters = {{"type", "object"},
                  {"properties",
                   {{"pattern", {{"type", "string"}, {"description", "Regex pattern to search for"}}},
                    {"path", {{"type", "string"}, {"description", "File or directory to search in (defaults to .)"}}}}},
                  {"required", {"pattern"}}};
  t.execute = [safe_dir](const json& args) -> Result<std::string> {
    auto pattern = args.value("pattern", std::string());
    if (pattern.empty()) {
      return std::unexpected(std::string("pattern is required"));
    }

    auto raw_path = args.value("path", std::string("."));
    auto resolved = resolve_path(raw_path, safe_dir);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }

    std::regex re(pattern);
    std::string result;
    int count = 0;
    const int max_results = 50;

    auto search_file = [&](const std::filesystem::path& p) {
      std::ifstream file(p);
      if (!file.is_open())
        return;
      std::string line;
      int line_num = 0;
      while (std::getline(file, line) && count < max_results) {
        line_num++;
        try {
          if (std::regex_search(line, re)) {
            result += p.string() + ":" + std::to_string(line_num) + ": " + line + '\n';
            count++;
          }
        } catch (const std::regex_error&) {
          // skip lines that cause regex errors
        }
      }
    };

    std::error_code ec;
    auto status = std::filesystem::status(*resolved, ec);
    if (ec) {
      return std::unexpected("Cannot access path: " + *resolved);
    }

    if (std::filesystem::is_regular_file(status)) {
      search_file(*resolved);
    } else if (std::filesystem::is_directory(status)) {
      auto it = std::filesystem::recursive_directory_iterator(*resolved, std::filesystem::directory_options::skip_permission_denied, ec);
      auto end = std::filesystem::recursive_directory_iterator{};
      for (; it != end && count < max_results; it.increment(ec)) {
        if (g_interrupted) {
          break;
        }
        if (it->path().filename() == ".git" && it->is_directory()) {
          it.disable_recursion_pending();
          continue;
        }
        if (it->is_regular_file()) {
          search_file(it->path());
        }
      }
    } else {
      return std::unexpected("Not a file or directory: " + *resolved);
    }

    if (result.empty()) {
      result = "(no matches)";
    }
    return result;
  };
  return t;
}

static Tool make_write_file_tool(const std::string& safe_dir) {
  Tool t;
  t.name = "write_file";
  t.description = "Write content to a file, creating parent directories if needed";
  t.parameters = {{"type", "object"},
                  {"properties", {{"path", {{"type", "string"}, {"description", "File path"}}}, {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
                  {"required", {"path", "content"}}};
  t.execute = [safe_dir](const json& args) -> Result<std::string> {
    auto raw = args.value("path", std::string());
    auto content = args.value("content", std::string());

    auto resolved = resolve_path(raw, safe_dir);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(*resolved).parent_path(), ec);
    if (ec) {
      return std::unexpected("Failed to create parent directories: " + ec.message());
    }

    std::ofstream file(*resolved, std::ios::binary);
    if (!file.is_open()) {
      return std::unexpected("Failed to write file: " + *resolved);
    }
    file.write(content.data(), content.size());
    file.close();

    return "ok (" + std::to_string(content.size()) + " bytes written)";
  };
  return t;
}

static Tool make_run_bash_tool(const std::string& safe_dir) {
  Tool t;
  t.name = "run_bash";
  t.description = "Run a bash command in the project directory "
                  "(e.g. build, test, lint). Output is capped at 100 lines / 4000 chars.";
  t.timeout_sec = 30;
  t.parameters = {{"type", "object"}, {"properties", {{"command", {{"type", "string"}, {"description", "Shell command to execute"}}}}}, {"required", {"command"}}};
  t.execute = [safe_dir](const json& args) -> Result<std::string> {
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

      setpgid(0, 0); // new process group

      if (!safe_dir.empty()) {
        chdir(safe_dir.c_str());
      }

      execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
      _exit(127);
    }

    // ---- parent ----
    close(pipefd[1]);
    setpgid(pid, pid); // eliminate race with child's setpgid(0,0)

    std::string output;
    char buf[4096];
    const int timeout_secs = [&] {
      const char* e = std::getenv("LLM_BASH_TIMEOUT");
      return e ? std::atoi(e) : 30;
    }();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);

    // Set read end to non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags != -1) {
      fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    }

    auto kill_child = [&] {
      killpg(pid, SIGKILL);
      int st;
      waitpid(pid, &st, 0);
    };

    auto truncate_output = [](std::string& out) {
      if (out.size() > 4000) {
        out = out.substr(0, 4000) + "...(truncated, >4000 chars)";
      }
    };

    while (true) {
      if (g_interrupted) {
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
        if (output.size() > 4000) {
          kill_child();
          close(pipefd[0]);
          output = output.substr(0, 4000) + "...(truncated, >4000 chars)";
          return output;
        }
      } else if (n == 0) {
        break; // EOF
      } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        struct pollfd pfd = {pipefd[0], POLLIN, 0};
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
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
    if (nl > 100) {
      size_t pos = 0;
      for (int i = 0; i < 100; i++) {
        pos = output.find('\n', pos) + 1;
      }
      output = output.substr(0, pos) + "...(truncated, >100 lines)";
    }

    // Final size cap
    truncate_output(output);

    return output;
  };
  return t;
}

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

void ToolRegistry::add_defaults(const std::string& safe_dir) {
  add(make_list_files_tool(safe_dir));
  add(make_read_file_tool(safe_dir));
  add(make_grep_files_tool(safe_dir));
  add(make_write_file_tool(safe_dir));
  add(make_run_bash_tool(safe_dir));
}

json ToolRegistry::to_openai_tools() const {
  json arr = json::array();
  for (const auto& t : tools_) {
    arr.push_back({{"type", "function"}, {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
  }
  return arr;
}

Result<std::string> ToolRegistry::execute(const std::string& name, const std::string& args_json) {
  Tool* tool = find(name);
  if (!tool) {
    return std::unexpected("unknown tool: " + name);
  }

  json args;
  try {
    args = json::parse(args_json);
  } catch (const json::parse_error& e) {
    return std::unexpected("invalid JSON arguments: " + std::string(e.what()));
  }

  if (tool->timeout_sec > 0) {
    auto future = std::async(std::launch::async, [tool, args = std::move(args)] {
      return tool->execute(args);
    });
    auto status = future.wait_for(std::chrono::seconds(tool->timeout_sec));
    if (status == std::future_status::timeout) {
      return std::unexpected("tool '" + tool->name + "' timed out after " + std::to_string(tool->timeout_sec) + "s");
    }
    return future.get();
  }

  return tool->execute(args);
}

Tool* ToolRegistry::find(const std::string& name) {
  for (auto& t : tools_) {
    if (t.name == name)
      return &t;
  }
  return nullptr;
}
