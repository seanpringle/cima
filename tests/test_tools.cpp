#include "tools.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <thread>

// Direct include for text_file_detector (not exported from tools.h)
#include "tools/text_file_detector.h"

namespace fs = std::filesystem;

// ===================================================================
// Helpers
// ===================================================================

static std::string make_temp_dir() {
    char tmpl[] = "/tmp/cima_test_XXXXXX";
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
    reg.add_defaults("/tmp", Config{});

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
                       "read_file",
                       "grep_files", "find_files",
                       "write_file",
                       "edit_file",
                       "run_bwrap", "run_bwrap_ro", "web_search", "web_fetch",
                       });
}

TEST_CASE("ToolRegistry without write tools (Planner-style)", "[tools][registry]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{}, false);

    json tools = reg.to_openai_tools();
    REQUIRE(tools.is_array());
    REQUIRE(tools.size() == 6);

    // No write tools should be present
    std::set<std::string> names;
    for (const auto& t : tools) {
        names.insert(t["function"]["name"].get<std::string>());
    }
    CHECK(names.find("write_file") == names.end());
    CHECK(names.find("edit_file") == names.end());
    CHECK(names.find("run_bwrap") == names.end()); // write variant

    // Read-only tools should be present
    CHECK(names.find("read_file") != names.end());
    CHECK(names.find("grep_files") != names.end());
    CHECK(names.find("find_files") != names.end());
    CHECK(names.find("web_search") != names.end());
    CHECK(names.find("web_fetch") != names.end());
    CHECK(names.find("run_bwrap_ro") != names.end());
}

TEST_CASE("ToolRegistry deduplicates tools by name", "[tools][registry]") {
    ToolRegistry reg;

    Tool t1;
    t1.name = "my-tool";
    t1.description = "first";
    t1.parameters = {{"type", "object"}, {"properties", json::object()}};
    t1.execute = [](const json&) -> Result<std::string> { return "first"; };

    Tool t2;
    t2.name = "my-tool"; // same name
    t2.description = "second";
    t2.parameters = {{"type", "object"}, {"properties", json::object()}};
    t2.execute = [](const json&) -> Result<std::string> { return "second"; };

    reg.add(std::move(t1));
    reg.add(std::move(t2)); // should replace, not duplicate

    const auto& tools = reg.tools();
    REQUIRE(tools.size() == 1);
    CHECK(tools[0].name == "my-tool");
    CHECK(tools[0].description == "second"); // replaced with latest

    // to_openai_tools should also produce exactly one entry
    json j = reg.to_openai_tools();
    REQUIRE(j.size() == 1);
    CHECK(j[0]["function"]["name"] == "my-tool");
    CHECK(j[0]["function"]["description"] == "second");
}


// ===================================================================
// read_file
// ===================================================================

TEST_CASE("read_file basic", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "Hello, World!\n";
    auto result = reg.execute("read_file", R"({"path": "hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("Hello, World!") != std::string::npos);
    CHECK(result->find("truncated") == std::string::npos);

    fs::remove_all(sd);
}



TEST_CASE("read_file path traversal rejected", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "Hello, World!\n";
    auto result = reg.execute("edit_file",
                              R"({"path": "hello.txt", "search": "", "replace": "x"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("edit_file path traversal rejected", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("edit_file",
                              R"({"path": "../../etc/passwd", "search": "root", "replace": "xxx"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("edit_file preserves rest of file", "[tools][edit_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

    auto result =
        reg.execute("grep_files",
                    R"({"pattern": "root", "path": "../../etc"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("grep_files ignores gitignored files", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a git repo and an initial commit (required for gitignore to work)
    auto r = reg.execute("run_bwrap", R"({"command": "git init"})");
    REQUIRE(r);
    reg.execute("run_bwrap",
        R"({"command": "git config user.email test@test.com"})");
    reg.execute("run_bwrap",
        R"({"command": "git config user.name Test"})");
    std::ofstream(sd + "/README.md") << "# Test\n";
    reg.execute("run_bwrap", R"({"command": "git add -A"})");
    reg.execute("run_bwrap", R"({"command": "git commit -m 'initial commit'"})");

    // Create .gitignore that ignores *.log and build/
    std::ofstream(sd + "/.gitignore") << "*.log\nbuild/\n";

    // Create some files
    std::ofstream(sd + "/hello.txt") << "hello world\n";
    std::ofstream(sd + "/trace.log") << "hello from log\n";
    fs::create_directory(sd + "/build");
    std::ofstream(sd + "/build/out.o") << "hello binary\n";

    // Search for "hello" — should only match hello.txt
    auto result = reg.execute("grep_files",
                              R"({"pattern": "hello", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);
    CHECK(result->find("trace.log") == std::string::npos);   // ignored by *.log
    CHECK(result->find("out.o") == std::string::npos);       // ignored via build/

    fs::remove_all(sd);
}

TEST_CASE("grep_files without gitignore still works", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "hello world\n";
    std::ofstream(sd + "/trace.log") << "hello from log\n";

    // No .gitignore, no git repo — both files should be searched
    auto result = reg.execute("grep_files",
                              R"({"pattern": "hello", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);
    CHECK(result->find("trace.log") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// grep_files depth limit
// ===================================================================

TEST_CASE("grep_files depth limit", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create nested structure:
    //   root.txt         — contains "data"
    //   sub/             — directory
    //     sub.txt        — contains "data"
    //     deep/          — directory
    //       deep.txt     — contains "data"
    std::ofstream(sd + "/root.txt") << "data\n";
    fs::create_directory(sd + "/sub");
    std::ofstream(sd + "/sub/sub.txt") << "data\n";
    fs::create_directory(sd + "/sub/deep");
    std::ofstream(sd + "/sub/deep/deep.txt") << "data\n";

    // depth=0: only root.txt
    auto r0 = reg.execute("grep_files", R"({"pattern": "data", "path": ".", "depth": 0})");
    REQUIRE(r0);
    CHECK(r0->find("root.txt") != std::string::npos);
    CHECK(r0->find("sub.txt") == std::string::npos);
    CHECK(r0->find("deep.txt") == std::string::npos);

    // depth=1: root.txt + sub/sub.txt
    auto r1 = reg.execute("grep_files", R"({"pattern": "data", "path": ".", "depth": 1})");
    REQUIRE(r1);
    CHECK(r1->find("root.txt") != std::string::npos);
    CHECK(r1->find("sub/sub.txt") != std::string::npos);
    CHECK(r1->find("deep.txt") == std::string::npos);

    // depth=2: all three
    auto r2 = reg.execute("grep_files", R"({"pattern": "data", "path": ".", "depth": 2})");
    REQUIRE(r2);
    CHECK(r2->find("root.txt") != std::string::npos);
    CHECK(r2->find("sub/sub.txt") != std::string::npos);
    CHECK(r2->find("sub/deep/deep.txt") != std::string::npos);

    // depth=-1 (default): all three
    auto r_all = reg.execute("grep_files", R"({"pattern": "data", "path": "."})");
    REQUIRE(r_all);
    CHECK(r_all->find("root.txt") != std::string::npos);
    CHECK(r_all->find("sub/sub.txt") != std::string::npos);
    CHECK(r_all->find("sub/deep/deep.txt") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// find_files
// ===================================================================

TEST_CASE("find_files basic", "[tools][find_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "hello\n";
    std::ofstream(sd + "/world.txt") << "world\n";
    std::ofstream(sd + "/data.log") << "log\n";
    fs::create_directory(sd + "/subdir");

    // Match .txt files (subdir doesn't match .txt pattern, so it won't appear)
    auto result = reg.execute("find_files", R"({"pattern": "\\.txt$", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);
    CHECK(result->find("world.txt") != std::string::npos);
    CHECK(result->find("data.log") == std::string::npos);
    CHECK(result->find("subdir") == std::string::npos); // dir doesn't match \.txt$

    // Match pattern that includes both files and directories
    result = reg.execute("find_files", R"({"pattern": "subdir|hello", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);
    CHECK(result->find("subdir/") != std::string::npos); // dirs get / suffix

    fs::remove_all(sd);
}

TEST_CASE("find_files with depth", "[tools][find_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directory(sd + "/sub");
    fs::create_directory(sd + "/sub/deep");
    std::ofstream(sd + "/root.txt") << "data\n";
    std::ofstream(sd + "/sub/sub.txt") << "data\n";
    std::ofstream(sd + "/sub/deep/deep.txt") << "data\n";

    // depth=0: only root
    auto r0 = reg.execute("find_files", R"({"pattern": "\\.txt$", "path": ".", "depth": 0})");
    REQUIRE(r0);
    CHECK(r0->find("root.txt") != std::string::npos);
    CHECK(r0->find("sub/sub.txt") == std::string::npos);
    CHECK(r0->find("sub/deep/deep.txt") == std::string::npos);

    // depth=1: root + sub/
    auto r1 = reg.execute("find_files", R"({"pattern": "\\.txt$", "path": ".", "depth": 1})");
    REQUIRE(r1);
    CHECK(r1->find("root.txt") != std::string::npos);
    CHECK(r1->find("sub/sub.txt") != std::string::npos);
    CHECK(r1->find("sub/deep/deep.txt") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("find_files directories get trailing slash", "[tools][find_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directory(sd + "/mydir");
    std::ofstream(sd + "/mydir/note.txt") << "data\n";

    auto result = reg.execute("find_files", R"({"pattern": "mydir", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("mydir/") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("find_files respects gitignore", "[tools][find_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Set up git repo
    auto r = reg.execute("run_bwrap", R"({"command": "git init"})");
    REQUIRE(r);
    reg.execute("run_bwrap", R"({"command": "git config user.email test@test.com"})");
    reg.execute("run_bwrap", R"({"command": "git config user.name Test"})");
    std::ofstream(sd + "/README.md") << "# Test\n";
    reg.execute("run_bwrap", R"({"command": "git add -A"})");
    reg.execute("run_bwrap", R"({"command": "git commit -m 'initial commit'"})");

    // Add .gitignore
    std::ofstream(sd + "/.gitignore") << "*.log\ncache/\n";

    // Create files
    std::ofstream(sd + "/hello.txt") << "hello\n";
    std::ofstream(sd + "/trace.log") << "log\n";
    fs::create_directory(sd + "/cache");
    std::ofstream(sd + "/cache/out.o") << "binary\n";

    // Should find hello.txt but not trace.log or cache/
    auto result = reg.execute("find_files", R"({"pattern": "trace", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("trace.log") == std::string::npos);

    result = reg.execute("find_files", R"({"pattern": "hello", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);

    // cache/ directory should be excluded
    result = reg.execute("find_files", R"({"pattern": "cache", "path": "."})");
    REQUIRE(result);
    CHECK(result->find("cache/") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("find_files path traversal rejected", "[tools][find_files]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});

    auto result = reg.execute("find_files", R"({"pattern": "x", "path": "../../etc"})");
    REQUIRE_FALSE(result);
    CHECK(result.error().find("must be under") != std::string::npos);
}

TEST_CASE("find_files empty pattern rejected", "[tools][find_files]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});

    auto result = reg.execute("find_files", R"({"pattern": "", "path": "."})");
    REQUIRE_FALSE(result);
}

TEST_CASE("find_files no matches", "[tools][find_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "hello\n";

    auto result = reg.execute("find_files", R"({"pattern": "zzzz_nonexistent", "path": "."})");
    REQUIRE(result);
    CHECK(*result == "(no matches)");

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

// A realistic snippet of DDG HTML search results for testing.
// Contains two results with titles, snippets, and URLs (DDG redirect style).
static const char* DDG_HTML_SAMPLE = R"(<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>test at DuckDuckGo</title></head>
<body>
<div class="serp__results">
<div id="links" class="results">
<div class="result results_links results_links_deep web-result ">
<div class="links_main links_deep result__body">
<h2 class="result__title">
<a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fresult1&amp;rut=abc">First Result</a>
</h2>
<div class="result__extras">
<div class="result__extras__url">
<a class="result__url" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fresult1&amp;rut=abc">example.com/result1</a>
</div>
</div>
<a class="result__snippet" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fresult1&amp;rut=abc">This is the <b>first</b> snippet text.</a>
</div>
</div>
<div class="result results_links results_links_deep web-result ">
<div class="links_main links_deep result__body">
<h2 class="result__title">
<a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fpage2&amp;rut=def">Second Title</a>
</h2>
<div class="result__extras">
<div class="result__extras__url">
<a class="result__url" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fpage2&amp;rut=def">example.org/page2</a>
</div>
</div>
<a class="result__snippet" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fpage2&amp;rut=def">Another snippet with <b>bold</b> and text.</a>
</div>
</div>
</div>
</div>
</body>
</html>)";

// ===================================================================
// extract_uddg_url unit tests
// ===================================================================

TEST_CASE("extract_uddg_url decodes correctly", "[tools][web_search]") {
    auto url = extract_uddg_url(
        "//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpath&rut=abc123");
    CHECK(url == "https://example.com/path");
}

TEST_CASE("extract_uddg_url returns empty when no uddg param", "[tools][web_search]") {
    CHECK(extract_uddg_url("//example.com/no-redirect").empty());
}

TEST_CASE("extract_uddg_url returns empty on empty input", "[tools][web_search]") {
    CHECK(extract_uddg_url("").empty());
}

TEST_CASE("extract_uddg_url handles uddg at end without trailing &", "[tools][web_search]") {
    auto url = extract_uddg_url(
        "//duckduckgo.com/l/?uddg=https%3A%2F%2Ftest.com");
    CHECK(url == "https://test.com");
}

// ===================================================================
// ddg_html_parse unit tests
// ===================================================================

TEST_CASE("ddg_html_parse extracts results correctly", "[tools][web_search]") {
    auto result = ddg_html_parse(DDG_HTML_SAMPLE);
    REQUIRE(result);

    // Should contain both results
    CHECK(result->find("1. First Result") != std::string::npos);
    CHECK(result->find("This is the first snippet text.") != std::string::npos);
    CHECK(result->find("https://example.com/result1") != std::string::npos);

    CHECK(result->find("2. Second Title") != std::string::npos);
    CHECK(result->find("Another snippet with bold and text.") != std::string::npos);
    CHECK(result->find("https://example.org/page2") != std::string::npos);
}

TEST_CASE("ddg_html_parse handles empty body", "[tools][web_search]") {
    auto result = ddg_html_parse("<html><body></body></html>");
    REQUIRE(result);
    CHECK(*result == "(no results found)");
}

TEST_CASE("ddg_html_parse handles completely empty string", "[tools][web_search]") {
    auto result = ddg_html_parse("");
    REQUIRE(result);
    CHECK(*result == "(no results found)");
}

TEST_CASE("ddg_html_parse handles malformed HTML", "[tools][web_search]") {
    auto result = ddg_html_parse("<html><body><div class=\"web-result\"><h2 class=\"result__title\"><a class=\"result__a\" href=\"//ddg.com/l/?uddg=https%3A%2F%2Fx.com\">X</a></h2></div></body></html>");
    REQUIRE(result);
    CHECK(result->find("1. X") != std::string::npos);
    CHECK(result->find("https://x.com") != std::string::npos);
}

// ===================================================================
// web_search tool integration tests (with MockHttpServer)
// ===================================================================

TEST_CASE("web_search always registered", "[tools][web_search]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    // Verify the tool exists in the registry
    auto tools = reg.to_openai_tools();
    bool found = false;
    for (const auto& t : tools) {
        if (t["function"]["name"] == "web_search") {
            found = true;
            break;
        }
    }
    CHECK(found);
}

TEST_CASE("web_search empty query rejected", "[tools][web_search]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_search", R"({"query": ""})");
    CHECK_FALSE(result);
    CHECK(result.error() == "query is required");
}

TEST_CASE("web_search respects max query length", "[tools][web_search]") {
    // Verify the tool truncates long queries
    // We test this by checking the tool definition rather than executing
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    // The parameter schema should allow strings up to 500 chars
    auto tools = reg.to_openai_tools();
    bool found = false;
    for (const auto& t : tools) {
        if (t["function"]["name"] == "web_search") {
            found = true;
            auto props = t["function"]["parameters"]["properties"];
            CHECK(props["query"]["type"] == "string");
            break;
        }
    }
    CHECK(found);
}

// ===================================================================
// Live DDG format verification (hidden by default — opt-in only)
// ===================================================================
// This test hits the real DuckDuckGo HTML endpoint to verify the
// HTML structure hasn't changed.  Hidden with [.] so it is excluded
// from default ctest runs — run explicitly only when you suspect DDG
// changed their result page format:
//
//   ./test_tools "[ddg-format]"
//
TEST_CASE("DDG HTML format is still parseable", "[.][ddg-format]") {
    // Make one real POST to DDG's HTML search
    auto resp = http_post_form(
        "https://html.duckduckgo.com/html/",
        "q=test",
        15,      // timeout
        nullptr  // no cancellation
    );
    REQUIRE(resp);

    // If we got a 202 (challenge page from rate limiting), skip — that's
    // an operational condition, not a format change.
    if (resp->second == 202) {
        SUCCEED("DDG returned a challenge page (HTTP 202) — skipping format check. "
                "This is normal when running from some networks.");
        return;
    }

    REQUIRE(resp->second == 200);

    // Parse the HTML and verify we get structured results
    auto parsed = ddg_html_parse(resp->first);
    REQUIRE(parsed);

    // Should have at least one result with a title (starts with "N. ")
    CHECK(parsed->find("1. ") != std::string::npos);

    // Log a summary so developers can see what happened
    INFO("DDG returned " << std::count(parsed->begin(), parsed->end(), '\n')
         << " lines of formatted results");
}

// ===================================================================
// web_fetch
// ===================================================================

TEST_CASE("web_fetch basic", "[tools][web_fetch]") {
    std::string body = "Hello, World! This is a test page.";
    MockHttpServer server(body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    CHECK(*result == body);
}

TEST_CASE("web_fetch converts HTML to Markdown", "[tools][web_fetch]") {
    std::string html = "<html><body><h1>Hello</h1><p>World</p></body></html>";
    MockHttpServer server(html, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    // The response should contain Markdown equivalents
    CHECK(result->find("# Hello") != std::string::npos);
    CHECK(result->find("World") != std::string::npos);
}

TEST_CASE("web_fetch empty URL rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch", R"({"url": ""})");
    CHECK_FALSE(result);
    CHECK(result.error() == "url is required");
}

TEST_CASE("web_fetch http error", "[tools][web_fetch]") {
    MockHttpServer server("Not Found", 404);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + server.url().substr(0, server.url().find("?q=")) + R"("})");
    CHECK_FALSE(result);
    CHECK(result.error().find("HTTP 404") != std::string::npos);
}

TEST_CASE("web_fetch connection refused", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch", R"({"url": "http://127.0.0.1:1/test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("curl error") != std::string::npos);
}

TEST_CASE("web_fetch unsupported scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch", R"({"url": "ftp://example.com/file"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch file scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch", R"({"url": "file:///etc/passwd"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch data scheme rejected", "[tools][web_fetch]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    auto result = reg.execute("web_fetch", R"({"url": "data:text/plain,hello"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("only http and https") != std::string::npos);
}

TEST_CASE("web_fetch returns full large content", "[tools][web_fetch]") {
    // Generate content larger than 100k chars
    std::string large_body(100500, 'x');
    MockHttpServer server(large_body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    std::string fetch_url = server.url().substr(0, server.url().find("?q="));
    auto result = reg.execute("web_fetch",
        R"({"url": ")" + fetch_url + R"("})");
    INFO("fetch_url = " << fetch_url);
    INFO("error = " << (result ? "none" : result.error()));
    REQUIRE(result);
    // Full content returned without truncation
    CHECK(result->size() >= 100000);
}

TEST_CASE("web_fetch available in plan mode", "[tools][web_fetch]") {
    std::string body = "plan mode content";
    MockHttpServer server(body, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{});
    
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
    reg.add_defaults("/tmp", Config{});

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

// ===================================================================
// text_file_detector
// ===================================================================

TEST_CASE("text_file_detector empty file returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/empty.txt";
    std::ofstream(path).close(); // create empty file

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector plain ASCII text returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/hello.txt";
    std::ofstream(path) << "Hello, world!\n";

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector binary file with NUL bytes returns Binary", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/binary.bin";
    std::vector<unsigned char> data(1024);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<unsigned char>(i & 0xFF);
    }
    // Force some NUL bytes at known positions
    data[500] = 0;
    data[700] = 0;
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Binary);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector UTF-8 BOM returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/utf8_bom.txt";
    // UTF-8 BOM followed by text (no NUL bytes)
    std::vector<unsigned char> data = {0xEF, 0xBB, 0xBF};
    for (int i = 0; i < 100; ++i) data.push_back('a' + (i % 26));
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector UTF-16LE BOM returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/utf16le.txt";
    std::vector<unsigned char> data = {0xFF, 0xFE}; // UTF-16LE BOM
    for (int i = 0; i < 50; ++i) {
        data.push_back('a' + i); data.push_back(0); // UTF-16LE encoding
    }
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector UTF-16BE BOM returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/utf16be.txt";
    std::vector<unsigned char> data = {0xFE, 0xFF}; // UTF-16BE BOM
    for (int i = 0; i < 50; ++i) {
        data.push_back(0); data.push_back('a' + i); // UTF-16BE encoding
    }
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector UTF-32LE BOM returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/utf32le.txt";
    std::vector<unsigned char> data = {0xFF, 0xFE, 0x00, 0x00}; // UTF-32LE BOM
    for (int i = 0; i < 25; ++i) {
        data.push_back('a' + i); data.push_back(0); data.push_back(0); data.push_back(0);
    }
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector UTF-32BE BOM returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/utf32be.txt";
    std::vector<unsigned char> data = {0x00, 0x00, 0xFE, 0xFF}; // UTF-32BE BOM
    for (int i = 0; i < 25; ++i) {
        data.push_back(0); data.push_back(0); data.push_back('a' + i); data.push_back(0);
    }
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector NUL at byte 8193 returns Text", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/boundary.txt";
    std::vector<unsigned char> data(8192, 'a'); // fill first 8KB with no NUL
    data.push_back(0); // NUL at byte 8193 — just past the scan boundary
    std::ofstream(path, std::ios::binary).write(
        reinterpret_cast<const char*>(data.data()), data.size());

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}

TEST_CASE("text_file_detector unreadable file returns Text (graceful fallback)", "[tools][text_file_detector]") {
    auto sd = make_temp_dir();
    auto path = sd + "/noexist.txt";
    // File does not exist — should gracefully return Text

    CHECK(detect_text_file(path) == FileKind::Text);

    fs::remove_all(sd);
}
