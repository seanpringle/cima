#include "tools.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <mutex>
#include <poll.h>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

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
    t.parameters = {{"type", "object"},
        {"properties", {{"path", {{"type", "string"}, {"description", "Directory path to list"}}}}},
        {"required", {"path"}}};
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

static Tool make_read_file_lines_tool(const std::string& safe_dir) {
    Tool t;
    t.name = "read_file_lines";
    t.description =
        "Read specific line ranges from a file. Returns lines prefixed with line "
        "numbers. Use this when you know the line numbers you want (e.g. after a "
        "grep match at line 52, read lines 45-78). "
        "For reading from an offset, use read_file instead.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file"}}},
                {"start_line",
                    {{"type", "integer"},
                        {"description",
                            "First line to read (1-indexed, default 1)"}}},
                {"end_line",
                    {{"type", "integer"},
                        {"description",
                            "Last line to read (inclusive). If omitted, reads to "
                            "end of file (capped by max_lines)."}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to return (default 200, max 500)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, safe_dir);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int start_line = args.value("start_line", 1);
        int end_line = args.value("end_line", 0); // 0 = not specified
        int max_lines = args.value("max_lines", 200);

        if (start_line < 1) {
            return std::unexpected("start_line must be >= 1");
        }
        if (end_line != 0 && end_line < start_line) {
            return std::unexpected("end_line must be >= start_line");
        }
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result;
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before start_line
        while (line_num < start_line - 1 && std::getline(file, line)) {
            line_num++;
        }

        // Determine how many lines to read
        int max_to_read = max_lines;
        if (end_line != 0) {
            int range = end_line - start_line + 1;
            if (range < max_to_read) max_to_read = range;
        }

        while (count < max_to_read && std::getline(file, line)) {
            line_num++;
            result += std::to_string(line_num) + ": " + line + "\n";
            count++;
        }

        // Check if there are more lines beyond what was read
        // Try reading one more line to detect EOF reliably
        bool has_more = false;
        int remaining = 0;
        if (end_line != 0 && line_num < end_line) {
            // We haven't read far enough per end_line — definitely has more
            has_more = true;
            std::string dummy;
            while (std::getline(file, dummy)) { remaining++; }
        } else {
            // Check if there's more content by attempting one more read
            std::string peek;
            has_more = static_cast<bool>(std::getline(file, peek));
            if (has_more) {
                remaining = 1;
                std::string dummy;
                while (std::getline(file, dummy)) { remaining++; }
            }
        }

        if (has_more) {
            result += "...(truncated, >" + std::to_string(count + remaining) +
                " lines from line " + std::to_string(start_line) + ")";
        } else if (count == 0 && start_line > 1) {
            result = "(start_line " + std::to_string(start_line) +
                " is beyond end of file)";
        }
        return result;
    };
    return t;
}

static Tool make_read_file_tool(const std::string& safe_dir) {
    Tool t;
    t.name = "read_file";
    t.description = "Read lines from a file (max 400 lines at a time, use offset to "
                    "paginate)";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file to read"}}},
                {"offset",
                    {{"type", "integer"},
                        {"description",
                            "Line number to start from (1-indexed, default 0 = "
                            "beginning)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to read starting from offset (default 200)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, safe_dir);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int offset = args.value("offset", 0);
        int max_lines = args.value("max_lines", 400);
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
            result += "...(truncated, >" + std::to_string(max_lines) + " lines from offset " +
                std::to_string(offset) + ")";
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
    t.description = "Search file contents using a regex pattern (max 200 results)";
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"pattern", {{"type", "string"}, {"description", "Regex pattern to search for"}}},
                {"path",
                    {{"type", "string"},
                        {"description", "File or directory to search in (defaults to .)"}}}}},
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
        const int max_results = 200;

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
            auto it = std::filesystem::recursive_directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec);
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
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path"}}},
                {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
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

static Tool make_edit_file_tool(const std::string& safe_dir) {
    Tool t;
    t.name = "edit_file";
    t.description = "Edit a file by searching for an exact string and replacing it. "
                    "The search string must match exactly once in the file — this ensures edits "
                    "are safe and unambiguous. "
                    "Use this to make targeted surgical edits instead of rewriting entire files "
                    "with write_file.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path to edit"}}},
                {"search",
                    {{"type", "string"},
                        {"description",
                            "Exact string to search for; must match exactly once in the file. "
                            "Include surrounding context (unique nearby lines) to guarantee a "
                            "single match."}}},
                {"replace",
                    {{"type", "string"},
                        {"description", "String to replace the matched occurrence with"}}}}},
        {"required", {"path", "search", "replace"}}};
    t.execute = [safe_dir](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto search = args.value("search", std::string());
        auto replace = args.value("replace", std::string());

        if (search.empty()) {
            return std::unexpected(std::string("search string is required"));
        }

        auto resolved = resolve_path(raw, safe_dir);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        // Read the file
        std::ifstream file(*resolved, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Failed to read file: " + *resolved);
        }
        std::string content(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // Count occurrences of the search string
        size_t count = 0;
        size_t pos = 0;
        while ((pos = content.find(search, pos)) != std::string::npos) {
            count++;
            pos += search.size();
        }

        if (count == 0) {
            return std::unexpected("Search string not found in file (0 matches). "
                                   "Use read_file or grep_files to verify the file contents.");
        }
        if (count > 1) {
            return std::unexpected("Search string found " + std::to_string(count) +
                " times in file (expected exactly 1). "
                "Include more surrounding context in the search string to uniquely identify the "
                "location.");
        }

        // Locate the unique occurrence
        pos = content.find(search);

        // Replace the search string with the replacement
        content.replace(pos, search.size(), replace);

        // Write the modified content back to the file
        std::ofstream out(*resolved, std::ios::binary);
        if (!out.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        out.write(content.data(), content.size());
        out.close();

        // Compute the line number where the edit occurred (1-indexed)
        int line_num = 1;
        for (size_t i = 0; i < pos; i++) {
            if (content[i] == '\n')
                line_num++;
        }

        return "ok (replaced 1 occurrence at line " + std::to_string(line_num) + ", " +
            std::to_string(search.size()) + " bytes -> " + std::to_string(replace.size()) +
            " bytes)";
    };
    return t;
}

static Tool make_run_bash_tool(const std::string& safe_dir) {
    Tool t;
    t.name = "run_bash";
    t.description = "Run a bash command in the project directory "
                    "(e.g. build, test, lint). Output is capped at 500 lines / 16000 chars.";
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"command", {{"type", "string"}, {"description", "Shell command to execute"}}}}},
        {"required", {"command"}}};
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
            if (out.size() > 16000) {
                out = out.substr(0, 16000) + "...(truncated, >16000 chars)";
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

        // Final size cap
        truncate_output(output);

        return output;
    };
    return t;
}

// ===================================================================
// Web search tool (libcurl)
// ===================================================================

static size_t web_search_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static int web_search_progress_cb(void* /*clientp*/,
    curl_off_t /*dltotal*/,
    curl_off_t /*dlnow*/,
    curl_off_t /*ultotal*/,
    curl_off_t /*ulnow*/) {
    return g_interrupted ? 1 : 0;
}

static Tool make_web_search_tool(const std::string& api_key,
    const std::string& engine_id,
    const std::string& endpoint_override) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web. Returns up to 10 results with titles, snippets, and URLs. "
        "By default uses the Wikipedia opensearch API (no key required). "
        "To use Google Custom Search, set SEARCH_API_KEY + SEARCH_ENGINE_ID. "
        "For a custom endpoint, set SEARCH_ENDPOINT with a {query} placeholder.";
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"query",
                {{"type", "string"}, {"description", "Search query (max 500 characters)"}}}}},
        {"required", {"query"}}};
    t.execute = [api_key, engine_id, endpoint_override](
                    const json& args) -> Result<std::string> {
        auto query = args.value("query", std::string());
        if (query.empty())
            return std::unexpected("query is required");

        if (query.size() > 500)
            query = query.substr(0, 500);

        // Determine which backend to use
        bool use_google = !api_key.empty() && !engine_id.empty();
        bool use_custom = !endpoint_override.empty();
        bool use_wikipedia = !use_google && !use_custom;

        // Build the request URL
        std::string url;
        std::string response_format_hint; // "google", "wikipedia", "custom"
        if (use_custom) {
            response_format_hint = "custom";
            url = endpoint_override;
            auto pos = url.find("{query}");
            if (pos != std::string::npos) {
                char* encoded = curl_easy_escape(nullptr, query.c_str(), (int)query.size());
                if (!encoded)
                    return std::unexpected("curl_easy_escape failed");
                url.replace(pos, 7, encoded);
                curl_free(encoded);
            }
        } else if (use_google) {
            response_format_hint = "google";
            char* enc_key = curl_easy_escape(nullptr, api_key.c_str(), 0);
            char* enc_cx = curl_easy_escape(nullptr, engine_id.c_str(), 0);
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_key || !enc_cx || !enc_q) {
                curl_free(enc_key);
                curl_free(enc_cx);
                curl_free(enc_q);
                return std::unexpected("curl_easy_escape failed");
            }
            url = "https://www.googleapis.com/customsearch/v1?key=" +
                std::string(enc_key) + "&cx=" + std::string(enc_cx) + "&q=" + std::string(enc_q);
            curl_free(enc_key);
            curl_free(enc_cx);
            curl_free(enc_q);
        } else {
            // Wikipedia opensearch fallback — no API key required
            response_format_hint = "wikipedia";
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_q)
                return std::unexpected("curl_easy_escape failed");
            url = "https://en.wikipedia.org/w/api.php?action=opensearch&search=" +
                std::string(enc_q) + "&format=json&limit=10&origin=*";
            curl_free(enc_q);
        }

        // libcurl GET
        CURL* curl = curl_easy_init();
        if (!curl)
            return std::unexpected("curl_easy_init failed");

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "llm-chat/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_search curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_search HTTP " + std::to_string(http_code);
            if (!body.empty())
                msg += ": " + body.substr(0, 500);
            return std::unexpected(msg);
        }

        // Parse JSON response
        json j;
        try {
            j = json::parse(body);
        } catch (const json::parse_error& e) {
            return std::unexpected("web_search JSON parse error: " + std::string(e.what()));
        }

        // Format results based on backend
        if (response_format_hint == "wikipedia") {
            // Wikipedia opensearch: ["query", ["title",...], ["desc",...], ["url",...]]
            if (!j.is_array() || j.size() < 4 || !j[1].is_array() || j[1].empty()) {
                return std::string("(no results found)");
            }
            std::string result;
            int rank = 1;
            for (size_t i = 0; i < j[1].size() && rank <= 10; i++) {
                std::string title = j[1][i].get<std::string>();
                std::string snippet = j[2].is_array() && i < j[2].size()
                    ? j[2][i].get<std::string>()
                    : "";
                std::string link = j[3].is_array() && i < j[3].size()
                    ? j[3][i].get<std::string>()
                    : "";
                result += std::to_string(rank) + ". " + title + "\n";
                if (!snippet.empty())
                    result += "   " + snippet + "\n";
                if (!link.empty())
                    result += "   " + link + "\n";
                result += "\n";
                rank++;
            }
            return result;
        } else {
            // Google CSE or custom endpoint: expects {"items": [...]}
            if (!j.contains("items") || !j["items"].is_array() || j["items"].empty()) {
                return std::string("(no results found)");
            }
            std::string result;
            int rank = 1;
            for (const auto& item : j["items"]) {
                if (rank > 10)
                    break;
                std::string title = item.value("title", "(no title)");
                std::string snippet = item.value("snippet", "");
                std::string link = item.value("link", "");
                result += std::to_string(rank) + ". " + title + "\n";
                if (!snippet.empty())
                    result += "   " + snippet + "\n";
                if (!link.empty())
                    result += "   " + link + "\n";
                result += "\n";
                rank++;
            }
            return result;
        }
    };
    return t;
}

// ===================================================================
// Web fetch tool (libcurl)
// ===================================================================

// ── URL validation helper ──

// Returns true if the scheme is http or https (case-insensitive).
// Rejects file://, ftp://, data:, javascript:, etc.
static bool is_valid_fetch_scheme(const std::string& url) {
    auto pos = url.find(':');
    if (pos == std::string::npos) return false;
    std::string scheme = url.substr(0, pos);
    for (auto& c : scheme) c = char(std::tolower((unsigned char)c));
    return scheme == "http" || scheme == "https";
}

// ── Caching ──

static std::mutex g_fetch_cache_mutex;
static std::unordered_map<std::string, std::string> g_fetch_cache;

// ── Tool factory ──

static Tool make_web_fetch_tool() {
    Tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch the content of a URL. Returns the response body as text. "
        "Max 100,000 characters. "
        "Use this to read documentation, API references, or web pages. "
        "Only returns the raw response body; for search results use web_search.";
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"url",
                {{"type", "string"},
                    {"description",
                        "URL to fetch (http/https). "
                        "Results are cached per session — re-fetching the same URL "
                        "returns the cached content."}}}}},
        {"required", {"url"}}};
    t.execute = [](const json& args) -> Result<std::string> {
        auto url = args.value("url", std::string());
        if (url.empty()) {
            return std::unexpected("url is required");
        }

        // ── URL validation ──
        // 1. Check scheme (http/https only)
        if (!is_valid_fetch_scheme(url)) {
            return std::unexpected("web_fetch: only http and https URLs are supported");
        }

        // ── Cache check ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            auto it = g_fetch_cache.find(url);
            if (it != g_fetch_cache.end()) {
                return it->second;
            }
        }

        // ── HTTP GET via libcurl ──
        CURL* curl = curl_easy_init();
        if (!curl) {
            return std::unexpected("web_fetch: curl_easy_init failed");
        }

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "llm-chat/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        // Read Content-Type BEFORE curl_easy_cleanup — the pointer is only
        // valid while the handle lives.
        std::string content_type;
        char* ct_raw = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_raw);
        if (ct_raw != nullptr) {
            content_type = ct_raw;
        }

        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_fetch curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_fetch HTTP " + std::to_string(http_code);
            if (!body.empty()) {
                msg += ": " + body.substr(0, 500);
            }
            return std::unexpected(msg);
        }

        // ── Content-Type filtering ──
        // Only allow text-based content types. If the server didn't send a
        // Content-Type header, we allow it (assume text).
        if (!content_type.empty()) {
            std::string ct = content_type;
            // Convert to lowercase for comparison
            for (auto& c : ct) c = char(std::tolower((unsigned char)c));

            // Strip parameters like charset=utf-8
            auto semi = ct.find(';');
            if (semi != std::string::npos) ct = ct.substr(0, semi);

            // Trim trailing whitespace
            while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t'))
                ct.pop_back();

            // Allowed content types
            bool allowed = false;
            if (ct.find("text/") == 0) allowed = true;
            else if (ct == "application/json") allowed = true;
            else if (ct == "application/xml") allowed = true;
            else if (ct == "application/javascript") allowed = true;
            else if (ct == "application/x-javascript") allowed = true;
            else if (ct == "application/atom+xml") allowed = true;
            else if (ct == "application/rss+xml") allowed = true;
            else if (ct == "application/xhtml+xml") allowed = true;

            if (!allowed) {
                return std::unexpected(
                    "web_fetch: unsupported Content-Type '" + ct +
                    "' — only text-based content can be fetched");
            }
        }

        // ── Truncation ──
        const size_t max_chars = 100000;
        if (body.size() > max_chars) {
            body = body.substr(0, max_chars) +
                "\n...(truncated, >" + std::to_string(max_chars) + " chars)";
        }

        // ── Cache the result ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            g_fetch_cache[url] = body;
        }

        return body;
    };
    return t;
}

// ===================================================================
// project_tree tool
// ===================================================================

static Tool make_project_tree_tool(const std::string& safe_dir) {
    Tool t;
    t.name = "project_tree";
    t.description =
        "Recursively list files/directories in a tree-like format.\n"
        "Maximum depth of 5 to avoid huge outputs. "
        "Use this to understand project structure in a single call "
        "instead of calling list_files repeatedly.";
    t.timeout_sec = 5;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description",
                        "Starting directory path (default '.')"}}},
                {"max_depth",
                    {{"type", "integer"},
                        {"description",
                            "Maximum recursion depth (default 5, max 10)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum output lines (default 500, max 500)"}}}}}};
    t.execute = [safe_dir](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string("."));
        auto resolved = resolve_path(raw, safe_dir);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int max_depth = args.value("max_depth", 5);
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 10) max_depth = 10;

        int max_lines = args.value("max_lines", 500);
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        std::error_code ec;
        auto status = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + *resolved);
        }

        // If the path is not a directory, just show it as a single line
        if (!std::filesystem::is_directory(status)) {
            return *resolved + "\n";
        }

        std::string result;
        int line_count = 0;
        bool truncated = false;
        bool interrupted = false;

        // Recursive walk — uses std::function for capture in lambda
        std::function<void(const std::filesystem::path&, int, const std::string&)> walk;
        walk = [&](const std::filesystem::path& dir, int depth, const std::string& prefix) {
            if (depth > max_depth) return;
            if (line_count >= max_lines) {
                truncated = true;
                return;
            }
            if (g_interrupted) {
                interrupted = true;
                return;
            }

            // Collect directory entries
            std::vector<std::filesystem::directory_entry> entries;
            std::error_code ec2;
            auto it = std::filesystem::directory_iterator(
                dir, std::filesystem::directory_options::skip_permission_denied, ec2);
            if (ec2) {
                // Cannot read this directory — skip silently
                return;
            }
            auto end = std::filesystem::directory_iterator{};
            for (; it != end; it.increment(ec2)) {
                if (g_interrupted) {
                    interrupted = true;
                    return;
                }
                if (ec2) {
                    // Permission error on one entry — skip it and continue
                    ec2.clear();
                    continue;
                }
                // Skip .git directory (don't traverse into it)
                if (it->path().filename() == ".git" && it->is_directory()) {
                    continue;
                }
                entries.push_back(*it);
            }

            // Sort: directories first, then files; alphabetically within each group
            std::sort(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) {
                    bool a_dir = a.is_directory();
                    bool b_dir = b.is_directory();
                    if (a_dir != b_dir) return a_dir > b_dir; // dirs first
                    // Compare filenames case-insensitively on the stored path
                    return a.path().filename().string() < b.path().filename().string();
                });

            for (size_t i = 0; i < entries.size(); i++) {
                if (g_interrupted) {
                    interrupted = true;
                    return;
                }
                if (line_count >= max_lines) {
                    truncated = true;
                    return;
                }

                const auto& entry = entries[i];
                bool is_last = (i == entries.size() - 1);

                // Build tree line
                result += prefix;
                result += is_last ? "└── " : "├── ";
                result += entry.path().filename().string();
                if (entry.is_directory()) {
                    result += "/";
                }
                result += "\n";
                line_count++;

                // Recurse into directories
                if (entry.is_directory() && depth < max_depth) {
                    std::string child_prefix = prefix + (is_last ? "    " : "│   ");
                    walk(entry.path(), depth + 1, child_prefix);
                }
            }
        };

        // Root line
        result += *resolved + "/\n";
        line_count++;

        // Walk
        walk(*resolved, 1, "");

        if (interrupted) {
            result += "(interrupted)\n";
        } else if (truncated) {
            result += "...(truncated, >" + std::to_string(max_lines) + " lines)\n";
        } else if (line_count <= 1) {
            // Only the root line — directory is empty (or all entries were skipped)
            // Try to detect if the directory actually has no visible entries
            // by counting non-.git items.
            std::error_code ec3;
            int count = 0;
            auto it2 = std::filesystem::directory_iterator(
                *resolved, std::filesystem::directory_options::skip_permission_denied, ec3);
            for (; it2 != std::filesystem::directory_iterator{}; it2.increment(ec3)) {
                if (it2->path().filename() != ".git") {
                    count++;
                    if (count > 0) break;
                }
            }
            if (count == 0) {
                result += "(empty directory)\n";
            } else {
                // Some entries exist but were skipped (e.g. permissions)
                // The tree output is already complete with what we could read.
            }
        }

        return result;
    };
    return t;
}

// ===================================================================
// ToolRegistry
// ===================================================================

void ToolRegistry::add(Tool tool) { tools_.push_back(std::move(tool)); }

void ToolRegistry::add_defaults(const std::string& safe_dir,
    const std::string& search_api_key,
    const std::string& search_engine_id,
    const std::string& search_endpoint) {
    add(make_list_files_tool(safe_dir));
    add(make_read_file_tool(safe_dir));
    add(make_read_file_lines_tool(safe_dir));
    add(make_grep_files_tool(safe_dir));
    add(make_write_file_tool(safe_dir));
    add(make_edit_file_tool(safe_dir));
    add(make_run_bash_tool(safe_dir));
    // Always register web_search — falls back to Wikipedia opensearch if no
    // credentials are configured. Google CSE or a custom endpoint can be used
    // by setting the appropriate environment variables.
    add(make_web_search_tool(search_api_key, search_engine_id, search_endpoint));
    add(make_project_tree_tool(safe_dir));
    add(make_web_fetch_tool());
}

json ToolRegistry::to_openai_tools() const {
    json arr = json::array();
    for (const auto& t : tools_) {
        arr.push_back({{"type", "function"},
            {"function",
                {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    }
    return arr;
}

Result<std::string> ToolRegistry::execute(const std::string& name, const std::string& args_json) {
    Tool* tool = find(name);
    if (!tool) {
        return std::unexpected("unknown tool: " + name);
    }

    // Plan mode restricts write/edit/bash at runtime
    if (mode_ == Mode::Plan &&
        (name == "write_file" || name == "edit_file" || name == "run_bash")) {
        return std::unexpected("Tool '" + name +
            "' is not available in Plan mode (read-only). "
            "Available tools: list_files, read_file, grep_files, project_tree, web_search, web_fetch.");
    }

    json args;
    try {
        args = json::parse(args_json);
    } catch (const json::parse_error& e) {
        return std::unexpected("invalid JSON arguments: " + std::string(e.what()));
    }

    if (tool->timeout_sec > 0) {
        auto future = std::async(
            std::launch::async, [tool, args = std::move(args)] { return tool->execute(args); });
        auto status = future.wait_for(std::chrono::seconds(tool->timeout_sec));
        if (status == std::future_status::timeout) {
            return std::unexpected("tool '" + tool->name + "' timed out after " +
                std::to_string(tool->timeout_sec) + "s");
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
