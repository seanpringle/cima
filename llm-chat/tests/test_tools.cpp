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
    REQUIRE(tools.size() == 5);

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
                       "run_bash"});
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

    // Generate 150 lines
    auto result = reg.execute(
        "run_bash", R"({"command": "for i in $(seq 1 150); do echo line $i; done"})");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);

    // Count lines in output
    int nl = 0;
    for (char c : *result)
        if (c == '\n')
            nl++;
    // Should be 100 + 1 (the truncation message)
    CHECK(nl <= 101);

    fs::remove_all(sd);
}

TEST_CASE("run_bash output size truncation", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // Generate output >4000 chars
    auto result = reg.execute(
        "run_bash",
        R"sh({"command": "for i in $(seq 1 500); do echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; done"})sh");
    REQUIRE(result);
    CHECK(result->find("truncated") != std::string::npos);
    CHECK(result->size() <= 4100);  // a bit of slop for the truncation msg

    fs::remove_all(sd);
}

TEST_CASE("run_bash path traversal in command is allowed (sandbox by cwd)",
          "[tools][run_bash]") {
    // run_bash runs in safe_dir, so the model could `cat ../../etc/passwd`
    // This is by design — the sandbox for run_bash is the CWD restriction,
    // not path checking (since it's a shell command). Other tools enforce paths.
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("run_bash", R"({"command": "pwd"})");
    REQUIRE(result);
    CHECK(result->find(sd) != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// Unknown tool
// ===================================================================

TEST_CASE("execute unknown tool returns error", "[tools][registry]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");

    auto result = reg.execute("nonexistent_tool", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("unknown tool") != std::string::npos);
}

TEST_CASE("execute with invalid JSON args returns error", "[tools][registry]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp");

    auto result = reg.execute("list_files", "not valid json");
    CHECK_FALSE(result);
    CHECK(result.error().find("invalid JSON") != std::string::npos);
}
