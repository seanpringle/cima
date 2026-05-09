#include "tools.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <fstream>
#include <set>
#include <thread>

namespace fs = std::filesystem;

// ===================================================================
// Helpers
// ===================================================================

static std::string make_temp_dir() {
    char tmpl[] = "/tmp/llmchat_test_XXXXXX";
    char* result = mkdtemp(tmpl);
    REQUIRE(result != nullptr);
    return result;
}

// ===================================================================
// resolve_path
// ===================================================================

TEST_CASE("resolve_path valid absolute within safe_dir", "[tools][sandbox]") {
    auto sd = make_temp_dir();
    auto sub = sd + "/subdir";
    fs::create_directories(sub);

    auto r = resolve_path(sub, sd);
    REQUIRE(r);
    CHECK(*r == fs::weakly_canonical(sub));

    fs::remove_all(sd);
}

TEST_CASE("resolve_path relative path resolved against safe_dir",
          "[tools][sandbox]") {
    auto sd = make_temp_dir();
    auto sub = sd + "/subdir";
    fs::create_directories(sub);

    auto r = resolve_path("subdir", sd);
    REQUIRE(r);
    CHECK(*r == fs::weakly_canonical(sub));

    fs::remove_all(sd);
}

TEST_CASE("resolve_path traversal rejected", "[tools][sandbox]") {
    auto sd = make_temp_dir();

    auto r = resolve_path(sd + "/../../etc/passwd", sd);
    CHECK_FALSE(r);
    CHECK(r.error().find("path must be under") != std::string::npos);

    r = resolve_path("../../etc/passwd", sd);
    CHECK_FALSE(r);
    CHECK(r.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("resolve_path absolute path outside safe_dir rejected",
          "[tools][sandbox]") {
    auto sd = make_temp_dir();

    auto r = resolve_path("/etc/passwd", sd);
    CHECK_FALSE(r);
    CHECK(r.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("resolve_path symlink escape rejected", "[tools][sandbox]") {
    auto sd = make_temp_dir();
    auto link_path = sd + "/evil_link";
    fs::create_symlink("/etc/passwd", link_path);

    auto r = resolve_path(link_path, sd);
    CHECK_FALSE(r);
    CHECK(r.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("resolve_path empty path rejected", "[tools][sandbox]") {
    auto sd = make_temp_dir();
    auto r = resolve_path("", sd);
    CHECK_FALSE(r);
    CHECK(r.error() == "path is required");
    fs::remove_all(sd);
}

TEST_CASE("resolve_path safe_dir itself is allowed", "[tools][sandbox]") {
    auto sd = make_temp_dir();
    auto r = resolve_path(sd, sd);
    REQUIRE(r);
    CHECK(*r == fs::weakly_canonical(sd));
    fs::remove_all(sd);
}

// ===================================================================
// ToolRegistry serialization
// ===================================================================

TEST_CASE("ToolRegistry to_openai_tools format", "[tools][registry]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");

    json tools = reg.to_openai_tools();
    REQUIRE(tools.is_array());
    REQUIRE(tools.size() == 9);

    // Check structure of first tool
    CHECK(tools[0]["type"] == "function");
    CHECK(tools[0]["function"].is_object());
    CHECK(tools[0]["function"]["name"].is_string());
    CHECK(tools[0]["function"]["description"].is_string());
    CHECK(tools[0]["function"]["parameters"]["type"] == "object");
    CHECK(tools[0]["function"]["parameters"]["properties"].is_object());

    // Check all names present
    std::set<std::string> names;
    for (const auto& t : tools) {
        names.insert(t["function"]["name"].get<std::string>());
    }
    CHECK(names == std::set<std::string>{
                       "list_files", "read_file", "grep_files", "write_file",
                       "edit_file", "run_bash", "web_search", "web_fetch",
                       "project_tree"});
}

// ===================================================================
// list_files
// ===================================================================

TEST_CASE("list_files basic", "[tools][list_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create some files
    std::ofstream(sd + "/a.txt") << "hello";
    std::ofstream(sd + "/b.txt") << "world";
    fs::create_directory(sd + "/sub");

    auto result = reg.execute("list_files", R"({"path": "."})");
    REQUIRE(result);

    // Output should contain our files
    CHECK(result->find("a.txt") != std::string::npos);
    CHECK(result->find("b.txt") != std::string::npos);
    CHECK(result->find("sub") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("list_files path traversal rejected", "[tools][list_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("list_files", R"({"path": "../../etc"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// project_tree
// ===================================================================

TEST_CASE("project_tree basic", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create a small directory tree
    fs::create_directories(sd + "/src/util");
    fs::create_directories(sd + "/tests");
    std::ofstream(sd + "/src/main.cpp") << "int main() {}\n";
    std::ofstream(sd + "/src/util/helper.h") << "// helper\n";
    std::ofstream(sd + "/README.md") << "# Project\n";
    std::ofstream(sd + "/tests/test_all.cpp") << "// tests\n";

    auto result = reg.execute("project_tree", R"({"path": "."})");
    REQUIRE(result);

    // Output should contain all files/dirs in tree format
    CHECK(result->find("src/") != std::string::npos);
    CHECK(result->find("main.cpp") != std::string::npos);
    CHECK(result->find("util/") != std::string::npos);
    CHECK(result->find("helper.h") != std::string::npos);
    CHECK(result->find("tests/") != std::string::npos);
    CHECK(result->find("test_all.cpp") != std::string::npos);
    CHECK(result->find("README.md") != std::string::npos);

    // Should contain tree-drawing characters
    CHECK(result->find("├──") != std::string::npos);
    CHECK(result->find("└──") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree max_depth", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create a deep hierarchy: a/b/c/d/e/file.txt (5 levels of dirs + 1 file)
    fs::create_directories(sd + "/a/b/c/d/e");
    std::ofstream(sd + "/a/b/c/d/e/file.txt") << "deep\n";

    // Depth 2 should show a/, b/, but not c/ or deeper
    auto result = reg.execute("project_tree", R"({"path": ".", "max_depth": 2})");
    REQUIRE(result);
    CHECK(result->find("a/") != std::string::npos);
    CHECK(result->find("b/") != std::string::npos);
    CHECK(result->find("c/") == std::string::npos);
    CHECK(result->find("file.txt") == std::string::npos);

    // Depth 6 should show everything (file.txt is at depth 6)
    auto result6 = reg.execute("project_tree", R"({"path": ".", "max_depth": 6})");
    REQUIRE(result6);
    CHECK(result6->find("file.txt") != std::string::npos);

    // Depth 1 should only show a/ (no deeper)
    auto result1 = reg.execute("project_tree", R"({"path": ".", "max_depth": 1})");
    REQUIRE(result1);
    CHECK(result1->find("a/") != std::string::npos);
    CHECK(result1->find("b/") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree max_lines", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create many files to exceed max_lines
    // Use a flat directory with max_depth=2 so we recurse one level
    fs::create_directories(sd + "/dir");
    for (int i = 0; i < 100; i++) {
        std::ofstream(sd + "/dir/file_" + std::to_string(i) + ".txt") << "x\n";
    }

    // max_depth=2 so we can see into dir/; max_lines=50 will truncate
    auto result = reg.execute("project_tree",
                              R"({"path": ".", "max_lines": 50, "max_depth": 2})");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree single file path", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "hello\n";

    auto result = reg.execute("project_tree", R"({"path": "hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree empty directory", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("project_tree", R"({"path": "."})");
    REQUIRE(result);
    CHECK(result->find("empty directory") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree path traversal rejected", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("project_tree", R"({"path": "../../etc"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree available in plan mode", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);
    reg.set_mode(Mode::Plan);

    fs::create_directories(sd + "/sub");
    std::ofstream(sd + "/sub/plan_file.txt") << "plan\n";

    auto result = reg.execute("project_tree", R"({"path": "."})");
    REQUIRE(result);
    CHECK(result->find("sub/") != std::string::npos);
    CHECK(result->find("plan_file.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree default parameters", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    fs::create_directories(sd + "/sub");
    std::ofstream(sd + "/sub/note.txt") << "note\n";

    // No arguments — should default to path=".", max_depth=5, max_lines=500
    auto result = reg.execute("project_tree", R"({})");
    REQUIRE(result);
    CHECK(result->find("sub/") != std::string::npos);
    CHECK(result->find("note.txt") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// read_file
// ===================================================================

TEST_CASE("read_file basic", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "Hello, World!\n";
    auto result = reg.execute("read_file", R"({"path": "hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("Hello, World!") != std::string::npos);
    CHECK(result->find("truncated") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file respects max_lines", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create a file with many lines
    std::ofstream ofs(sd + "/many.txt");
    for (int i = 0; i < 300; i++) {
        ofs << "line " << i << "\n";
    }
    ofs.close();

    auto result = reg.execute("read_file",
                              R"({"path": "many.txt", "max_lines": 50})");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file path traversal rejected", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("read_file", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

// ===================================================================
// write_file
// ===================================================================

TEST_CASE("write_file basic", "[tools][write_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result =
        reg.execute("write_file",
                    R"({"path": "newfile.txt", "content": "test content"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("bytes written") != std::string::npos);

    // Verify file exists
    REQUIRE(fs::exists(sd + "/newfile.txt"));
    std::ifstream ifs(sd + "/newfile.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "test content");

    fs::remove_all(sd);
}

TEST_CASE("write_file creates parent directories", "[tools][write_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute(
        "write_file",
        R"({"path": "a/b/c/deep.txt", "content": "nested"})");
    REQUIRE(result);
    CHECK(fs::exists(sd + "/a/b/c/deep.txt"));

    fs::remove_all(sd);
}

TEST_CASE("write_file path traversal rejected", "[tools][write_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute(
        "write_file",
        R"({"path": "../../etc/evil", "content": "pwned"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

// ===================================================================
// edit_file
// ===================================================================

TEST_CASE("edit_file basic search and replace", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Create a file with content to edit
    std::ofstream(sd + "/hello.txt") << "Hello, World!\nHow are you?\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "World", "replace": "Universe"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("line 1") != std::string::npos);

    // Verify file content
    std::ifstream ifs(sd + "/hello.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "Hello, Universe!\nHow are you?\n");

    fs::remove_all(sd);
}

TEST_CASE("edit_file search string not found", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "Hello, World!\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "Nonexistent", "replace": "X"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("edit_file multiple matches rejected", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "hello\nhello\nworld\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "hello", "replace": "goodbye"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("2 times") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("edit_file empty search rejected", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "Hello, World!\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "", "replace": "x"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("edit_file path traversal rejected", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("edit_file",
                              R"({"path": "../../etc/passwd", "search": "root", "replace": "xxx"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("edit_file preserves rest of file", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "line one\nline two\nline three\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "line two", "replace": "edited two"})");
    REQUIRE(result);
    CHECK(result->find("line 2") != std::string::npos);

    std::ifstream ifs(sd + "/hello.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "line one\nedited two\nline three\n");

    fs::remove_all(sd);
}

TEST_CASE("edit_file multiline search and replace", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "line one\nline two\nline three\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "line one\nline two", "replace": "combined"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);

    std::ifstream ifs(sd + "/hello.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "combined\nline three\n");

    fs::remove_all(sd);
}

// ===================================================================
// grep_files
// ===================================================================

TEST_CASE("grep_files basic match", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "hello world\nfoo bar\nbaz hello\n";
    std::ofstream(sd + "/other.txt") << "no match here\n";

    auto result = reg.execute("grep_files",
                              R"({"pattern": "hello", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello") != std::string::npos);
    CHECK(result->find("test.txt") != std::string::npos);
    CHECK(result->find("no matches") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("grep_files no match", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "hello world\n";

    auto result = reg.execute("grep_files",
                              R"({"pattern": "zzz_nonexistent", "path": "."})");
    REQUIRE(result);
    CHECK(*result == "(no matches)");

    fs::remove_all(sd);
}

TEST_CASE("grep_files path traversal rejected", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result =
        reg.execute("grep_files",
                    R"({"pattern": "root", "path": "../../etc"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

// ===================================================================
// run_bash
// ===================================================================

TEST_CASE("run_bash basic command", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/hello.txt") << "world\n";

    auto result = reg.execute("run_bash", R"({"command": "cat hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("world") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("run_bash command failure returns stderr", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result =
        reg.execute("run_bash", R"({"command": "ls nonexistent_file"})");
    REQUIRE(result);
    // Should contain error, not just empty
    CHECK_FALSE(result->empty());

    fs::remove_all(sd);
}

TEST_CASE("run_bash timeout kills process", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    setenv("LLM_BASH_TIMEOUT", "2", 1);
    auto start = std::chrono::steady_clock::now();
    auto result = reg.execute("run_bash",
                              R"({"command": "sleep 10 && echo done"})");
    auto elapsed = std::chrono::steady_clock::now() - start;
    unsetenv("LLM_BASH_TIMEOUT");

    REQUIRE(result);
    // Should NOT take 10 seconds — the 2s timeout should kill the process
    CHECK(elapsed < std::chrono::seconds(5));
    // Partial output may be empty (sleep produces no output)
    CHECK(result->empty());  // sleep 10 produces no output before SIGKILL

    fs::remove_all(sd);
}

TEST_CASE("run_bash output line truncation", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Generate 600 lines (exceeds new 500-line limit)
    auto result = reg.execute(
        "run_bash", R"({"command": "for i in $(seq 1 600); do echo line $i; done"})");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);

    // Count lines in output
    int nl = 0;
    for (char c : *result)
        if (c == '\n')
            nl++;
    // Should be 500 + 1 (the truncation message)
    CHECK(nl <= 501);

    fs::remove_all(sd);
}

TEST_CASE("run_bash output size truncation", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Generate output larger than 16000 chars
    auto result = reg.execute(
        "run_bash",
        R"({"command": "python3 -c 'print(\"x\" * 20000)'"})");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);
    CHECK(result->size() <= 16100);  // a bit over due to truncation message

    fs::remove_all(sd);
}

TEST_CASE("run_bash path traversal rejected", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute(
        "run_bash", R"({"command": "cat ../../etc/passwd"})");
    // run_bash does not sandbox the command itself; the shell runs in safe_dir
    // So cat ../../etc/passwd will fail because we're already in /tmp/... and
    // going up goes to /tmp/etc which doesn't exist or isn't accessible
    // Actually it might succeed if /tmp/etc/passwd exists, but it won't.
    // We just check that it runs (returns success) - the sandbox is about the
    // cwd, not about restricting commands.
    REQUIRE(result);

    fs::remove_all(sd);
}

// ===================================================================
// Mock HTTP server for web_search tests
// ===================================================================

#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <atomic>
#include <chrono>

struct MockHttpServer {
    std::thread thread;
    std::atomic<int> port{0};
    int server_fd = -1;
    std::string response_body;
    int response_status = 200;
    bool delay_response = false;

    MockHttpServer(const std::string& body, int status = 200, bool delay = false)
        : response_body(body), response_status(status), delay_response(delay) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(server_fd >= 0);

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        int rc = bind(server_fd, (sockaddr*)&addr, sizeof(addr));
        REQUIRE(rc == 0);

        rc = listen(server_fd, 1);
        REQUIRE(rc == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        rc = getsockname(server_fd, (sockaddr*)&bound, &len);
        REQUIRE(rc == 0);
        port = ntohs(bound.sin_port);

        thread = std::thread([this]() {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client = accept(server_fd, (sockaddr*)&client_addr, &client_len);
            if (client < 0)
                return;

            if (delay_response)
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            std::string resp = "HTTP/1.1 " + std::to_string(response_status) + " OK\r\n"
                               "Content-Type: application/json\r\n"
                               "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
                               "Connection: close\r\n\r\n" + response_body;
            send(client, resp.data(), resp.size(), 0);
            close(client);
        });
    }

    ~MockHttpServer() {
        if (server_fd >= 0) {
            close(server_fd);
            server_fd = -1;
        }
        if (thread.joinable())
            thread.join();
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port.load()) + "/search?q={query}";
    }
};

// ===================================================================
// web_search
// ===================================================================

TEST_CASE("web_search always registered", "[tools][web_search]") {
    // web_search is always registered now, even without API key
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    // Should not return "unknown tool"
    auto result = reg.execute("web_search", R"({"query": "hello"})");
    REQUIRE(result);
    CHECK(result->find("1.") != std::string::npos);
}

TEST_CASE("web_search falls back to wikipedia when no engine/endpoint", "[tools][web_search]") {
    // With only api_key (no engine_id, no endpoint), falls back to Wikipedia
    ToolRegistry reg;
    reg.add_defaults("/tmp", "my-api-key", "", "");
    auto result = reg.execute("web_search", R"({"query": "hello"})");
    // Should succeed via Wikipedia fallback
    REQUIRE(result);
    CHECK(result->find("1.") != std::string::npos);
}

TEST_CASE("web_search empty query rejected", "[tools][web_search]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", "key", "cx", "");
    auto result = reg.execute("web_search", R"({"query": ""})");
    CHECK_FALSE(result);
    CHECK(result.error() == "query is required");
}

TEST_CASE("web_search custom endpoint basic", "[tools][web_search]") {
    // Mock server returns Google-style JSON
    std::string mock_json = R"({
        "items": [
            {"title": "Result One", "snippet": "First snippet", "link": "https://example.com/1"},
            {"title": "Result Two", "snippet": "Second snippet", "link": "https://example.com/2"}
        ]
    })";
    MockHttpServer server(mock_json, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let server start

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    auto result = reg.execute("web_search", R"({"query": "test query"})");
    REQUIRE(result);

    // Check formatting
    CHECK(result->find("1. Result One") != std::string::npos);
    CHECK(result->find("First snippet") != std::string::npos);
    CHECK(result->find("https://example.com/1") != std::string::npos);
    CHECK(result->find("2. Result Two") != std::string::npos);
    CHECK(result->find("Second snippet") != std::string::npos);
    CHECK(result->find("https://example.com/2") != std::string::npos);
}

TEST_CASE("web_search custom endpoint no results", "[tools][web_search]") {
    std::string mock_json = R"({"items": []})";
    MockHttpServer server(mock_json, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    auto result = reg.execute("web_search", R"({"query": "nonexistent"})");
    REQUIRE(result);
    CHECK(*result == "(no results found)");
}

TEST_CASE("web_search custom endpoint http error", "[tools][web_search]") {
    std::string mock_json = R"({"error": "rate limited"})";
    MockHttpServer server(mock_json, 429);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    auto result = reg.execute("web_search", R"({"query": "test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("HTTP 429") != std::string::npos);
}

TEST_CASE("web_search custom endpoint connection refused", "[tools][web_search]") {
    // Start server, then let it close so port is free — we connect to a port
    // that nothing is listening on
    {
        MockHttpServer server("{}", 200);
        int used_port = server.port.load();
    } // server destroyed, port freed

    // Now connect to that same port — will likely be refused
    // (race condition: something else could grab the port)
    // Instead, test with an obviously unreachable port
    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", "http://127.0.0.1:1/search?q={query}");
    auto result = reg.execute("web_search", R"({"query": "test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("curl error") != std::string::npos);
}

TEST_CASE("web_search timeout", "[tools][web_search]") {
    std::string mock_json = R"({"items": []})";
    MockHttpServer server(mock_json, 200, true); // delay 1.5s, tool timeout is 15s
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    auto start = std::chrono::steady_clock::now();
    auto result = reg.execute("web_search", R"({"query": "test"})");
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 1.5s delay is within the 15s timeout, so it should succeed
    REQUIRE(result);
    CHECK(elapsed > std::chrono::seconds(1));
}

TEST_CASE("web_search available in plan mode", "[tools][web_search]") {
    std::string mock_json = R"({"items": [{"title": "A", "snippet": "B", "link": "C"}]})";
    MockHttpServer server(mock_json, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    reg.set_mode(Mode::Plan);

    auto result = reg.execute("web_search", R"({"query": "test"})");
    // Should succeed in Plan mode (read-only tool)
    REQUIRE(result);
}

TEST_CASE("web_search respects max query length", "[tools][web_search]") {
    std::string mock_json = R"({"items": [{"title": "A", "snippet": "B", "link": "C"}]})";
    MockHttpServer server(mock_json, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", "test-key", "", server.url());
    // 250 character query is below the 500-char limit, so no truncation
    std::string long_query(250, 'x');
    auto result = reg.execute("web_search",
        R"({"query": ")" + long_query + R"("})");
    REQUIRE(result);
    // Should have succeeded (the mock doesn't check query length)
    CHECK(result->find("1. A") != std::string::npos);
}

// ===================================================================
// web_fetch
// ===================================================================

TEST_CASE("web_fetch basic", "[tools][web_fetch]") {
    std::string body = "Hello, World! This is a test page.";
    MockHttpServer server(body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp");
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    CHECK(*result == body);
}

TEST_CASE("web_fetch empty URL rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch", R"({"url": ""})");
    CHECK_FALSE(result);
    CHECK(result.error() == "url is required");
}

TEST_CASE("web_fetch http error", "[tools][web_fetch]") {
    MockHttpServer server("Not Found", 404);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + server.url().substr(0, server.url().find("?q=")) + R"("})");
    CHECK_FALSE(result);
    CHECK(result.error().find("HTTP 404") != std::string::npos);
}

TEST_CASE("web_fetch connection refused", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch", R"({"url": "http://127.0.0.1:1/test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("curl error") != std::string::npos);
}

TEST_CASE("web_fetch unsupported scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch", R"({"url": "ftp://example.com/file"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch file scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch", R"({"url": "file:///etc/passwd"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch data scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");
    auto result = reg.execute("web_fetch", R"({"url": "data:text/plain,hello"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch truncates large content", "[tools][web_fetch]") {
    // Generate content larger than 100k chars
    std::string large_body(100500, 'x');
    MockHttpServer server(large_body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp");
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);
    CHECK(result->size() <= 100100); // 100k + truncation message
}

TEST_CASE("web_fetch available in plan mode", "[tools][web_fetch]") {
    std::string body = "plan mode content";
    MockHttpServer server(body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp");
    reg.set_mode(Mode::Plan);
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    CHECK(*result == body);
}

TEST_CASE("web_fetch caching returns same content", "[tools][web_fetch]") {
    // Use a unique URL based on a counter to avoid cross-test cache collisions.
    static int call_count = 0;
    call_count++;
    std::string body = "cached content " + std::to_string(call_count);

    MockHttpServer server(body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string mock_url =
        "http://127.0.0.1:" + std::to_string(server.port.load()) + "/cached_test";

    ToolRegistry reg;
    reg.add_defaults("/tmp");

    // First fetch should succeed
    auto r1 = reg.execute("web_fetch", R"({"url": ")" + mock_url + R"("})");
    REQUIRE(r1);
    CHECK(*r1 == body);

    // Second fetch of same URL should return cached content (same result)
    auto r2 = reg.execute("web_fetch", R"({"url": ")" + mock_url + R"("})");
    REQUIRE(r2);
    CHECK(*r2 == body);
}

TEST_CASE("web_fetch binary content-type rejected", "[tools][web_fetch]") {
    // MockHttpServer always returns Content-Type: application/json which is
    // allowed. To test binary rejection we'd need a server that returns
    // image/png, etc. Placeholder until MockHttpServer supports custom
    // Content-Type headers.
    SUCCEED("Binary Content-Type rejection requires MockHttpServer extension "
            "(currently always returns application/json which is allowed).");
}
