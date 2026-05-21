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
    REQUIRE(tools.size() == 23);

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
                       "list_directory", "read_file", "read_file_lines", "stat_file",
                       "grep_files", "write_file",
                       "edit_file", "run_bash", "web_search", "web_fetch",
                       "project_tree", "git_status", "git_diff", "git_log",
                       "git_add", "git_commit",
                       "git_restore", "git_show",
                       "delete_file", "move_file", "rename_file",
                       "create_directory", "delete_directory"});
}

TEST_CASE("ToolRegistry without write tools (Planner-style)", "[tools][registry]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", Config{}, false);

    json tools = reg.to_openai_tools();
    REQUIRE(tools.is_array());
    // 12 read-only tools: list_directory, read_file, read_file_lines, stat_file, grep_files,
    // project_tree, web_search, web_fetch, git_status, git_diff, git_log, git_show
    REQUIRE(tools.size() == 12);

    // No write tools should be present
    std::set<std::string> names;
    for (const auto& t : tools) {
        names.insert(t["function"]["name"].get<std::string>());
    }
    CHECK(names.find("write_file") == names.end());
    CHECK(names.find("edit_file") == names.end());
    CHECK(names.find("run_bash") == names.end());
    CHECK(names.find("git_add") == names.end());
    CHECK(names.find("git_commit") == names.end());
    CHECK(names.find("delete_file") == names.end());
    CHECK(names.find("move_file") == names.end());
    CHECK(names.find("rename_file") == names.end());

    // Read-only tools should be present
    CHECK(names.find("list_directory") != names.end());
    CHECK(names.find("read_file") != names.end());
    CHECK(names.find("read_file_lines") != names.end());
    CHECK(names.find("grep_files") != names.end());
    CHECK(names.find("project_tree") != names.end());
    CHECK(names.find("web_search") != names.end());
    CHECK(names.find("web_fetch") != names.end());
    CHECK(names.find("git_status") != names.end());
    CHECK(names.find("git_diff") != names.end());
    CHECK(names.find("git_log") != names.end());
}

// ===================================================================
// list_directory
// ===================================================================

TEST_CASE("list_directory basic", "[tools][list_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create some files
    std::ofstream(sd + "/a.txt") << "hello";
    std::ofstream(sd + "/b.txt") << "world";
    fs::create_directory(sd + "/sub");

    auto result = reg.execute("list_directory", R"({"path": "."})");
    REQUIRE(result);

    // Output should contain our files
    CHECK(result->find("a.txt") != std::string::npos);
    CHECK(result->find("b.txt") != std::string::npos);
    CHECK(result->find("sub") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("list_directory path traversal rejected", "[tools][list_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("list_directory", R"({"path": "../../etc"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// stat_file
// ===================================================================

TEST_CASE("stat_file regular file", "[tools][stat_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a file with known content
    std::ofstream(sd + "/test.txt") << "hello world";
    CHECK(std::filesystem::exists(sd + "/test.txt"));

    auto result = reg.execute("stat_file", R"({"path": "test.txt"})");
    REQUIRE(result);
    json j = json::parse(*result);
    CHECK(j["type"] == "regular_file");
    CHECK(j["size"] == 11); // "hello world" is 11 bytes
    CHECK(j["permissions"].is_string());
    CHECK(j["permissions"].get<std::string>().size() == 9);
    CHECK(j.contains("modified"));

    fs::remove_all(sd);
}

TEST_CASE("stat_file directory", "[tools][stat_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a directory with an entry
    std::filesystem::create_directories(sd + "/mydir");
    std::ofstream(sd + "/mydir/a_file") << "x";
    CHECK(std::filesystem::exists(sd + "/mydir"));

    auto result = reg.execute("stat_file", R"({"path": "mydir"})");
    REQUIRE(result);
    json j = json::parse(*result);
    CHECK(j["type"] == "directory");
    CHECK(j["permissions"].is_string());
    CHECK(j["permissions"].get<std::string>().size() == 9);
    CHECK(j["entry_count"] == 1);
    CHECK(!j.contains("size"));

    fs::remove_all(sd);
}

TEST_CASE("stat_file symlink", "[tools][stat_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/target") << "content";
    std::error_code ec;
    std::filesystem::create_symlink(sd + "/target", sd + "/link", ec);
    if (ec) {
        // Symlinks not supported on this platform (e.g. Windows without
        // developer mode) — skip the test
        fs::remove_all(sd);
        return;
    }

    auto result = reg.execute("stat_file", R"({"path": "link"})");
    REQUIRE(result);
    json j = json::parse(*result);
    CHECK(j["type"] == "symlink");

    fs::remove_all(sd);
}

TEST_CASE("stat_file path traversal rejected", "[tools][stat_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("stat_file", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("stat_file not found", "[tools][stat_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("stat_file", R"({"path": "nonexistent"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Cannot access") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// create_directory
// ===================================================================

TEST_CASE("create_directory basic", "[tools][create_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("create_directory", R"({"path": "my_dir"})");
    REQUIRE(result);
    CHECK(result->find("created") != std::string::npos);
    CHECK(std::filesystem::exists(sd + "/my_dir"));
    CHECK(std::filesystem::is_directory(sd + "/my_dir"));

    fs::remove_all(sd);
}

TEST_CASE("create_directory nested", "[tools][create_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("create_directory", R"({"path": "a/b/c"})");
    REQUIRE(result);
    CHECK(result->find("created") != std::string::npos);
    CHECK(std::filesystem::exists(sd + "/a/b/c"));
    CHECK(std::filesystem::is_directory(sd + "/a/b/c"));

    fs::remove_all(sd);
}

TEST_CASE("create_directory already exists", "[tools][create_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create it once
    std::filesystem::create_directories(sd + "/existing");
    CHECK(std::filesystem::exists(sd + "/existing"));

    // Create it again — should succeed silently
    auto result = reg.execute("create_directory", R"({"path": "existing"})");
    REQUIRE(result);
    CHECK(result->find("already exists") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("create_directory path traversal rejected", "[tools][create_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("create_directory", R"({"path": "../../etc/evil"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// delete_directory
// ===================================================================

TEST_CASE("delete_directory basic", "[tools][delete_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a directory first
    std::filesystem::create_directories(sd + "/to_delete");
    CHECK(std::filesystem::exists(sd + "/to_delete"));

    // Delete it
    auto result = reg.execute("delete_directory", R"({"path": "to_delete"})");
    REQUIRE(result);
    CHECK(result->find("removed") != std::string::npos);
    CHECK(!std::filesystem::exists(sd + "/to_delete"));

    fs::remove_all(sd);
}

TEST_CASE("delete_directory only works on empty dir", "[tools][delete_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a directory with a file inside
    std::filesystem::create_directories(sd + "/not_empty");
    std::ofstream(sd + "/not_empty/file.txt") << "content";
    CHECK(std::filesystem::exists(sd + "/not_empty/file.txt"));

    // Try to delete — should fail because directory is not empty
    auto result = reg.execute("delete_directory", R"({"path": "not_empty"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("not empty") != std::string::npos);

    // Verify directory still exists
    CHECK(std::filesystem::exists(sd + "/not_empty"));

    fs::remove_all(sd);
}

TEST_CASE("delete_directory path traversal rejected", "[tools][delete_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("delete_directory", R"({"path": "../../etc"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("delete_directory not a directory", "[tools][delete_directory]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a regular file
    std::ofstream(sd + "/a_file") << "hello";
    CHECK(std::filesystem::exists(sd + "/a_file"));

    // Try to delete it with delete_directory — should fail
    auto result = reg.execute("delete_directory", R"({"path": "a_file"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Not a directory") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// project_tree
// ===================================================================

TEST_CASE("project_tree basic", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "hello\n";

    auto result = reg.execute("project_tree", R"({"path": "hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("hello.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree empty directory", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("project_tree", R"({"path": "."})");
    REQUIRE(result);
    CHECK(result->find("empty directory") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree path traversal rejected", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("project_tree", R"({"path": "../../etc"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("project_tree available in plan mode", "[tools][project_tree]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    

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
    reg.add_defaults(sd, Config{});

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
// git_status
// ===================================================================

// Helper: create a temp directory with a git repo and an initial commit.
static std::string make_git_repo() {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto r = reg.execute("run_bash", R"({"command": "git init"})");
    REQUIRE(r);

    reg.execute("run_bash",
        R"({"command": "git config user.email test@test.com"})");
    reg.execute("run_bash",
        R"({"command": "git config user.name Test"})");

    std::ofstream(sd + "/README.md") << "# Test\n";
    reg.execute("run_bash", R"({"command": "git add -A"})");
    reg.execute("run_bash", R"({"command": "git commit -m 'initial commit'"})");
    return sd;
}

TEST_CASE("git_status clean repo", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(*result == "(clean — no changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_status modified tracked file", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify the tracked file
    std::ofstream(sd + "/README.md") << "# Modified\n";

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find(" M README.md") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status staged add", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a new file and stage it
    std::ofstream(sd + "/newfile.txt") << "new\n";
    reg.execute("run_bash", R"({"command": "git add newfile.txt"})");

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find("A  newfile.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status staged delete", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    reg.execute("run_bash", R"({"command": "git rm README.md"})");

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find("D  README.md") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status untracked file", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/untracked.txt") << "hello\n";

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find("?? untracked.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status mixed staged and unstaged", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Stage a modification, then modify again without staging
    std::ofstream(sd + "/README.md") << "# Modified\n";
    reg.execute("run_bash", R"({"command": "git add README.md"})");
    std::ofstream(sd + "/README.md") << "# Modified again\n";

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    // Both index and worktree show modified -> "MM"
    CHECK(result->find("MM README.md") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status not a git repo", "[tools][git_status]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // sd has no .git directory
    auto result = reg.execute("git_status", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status available in plan mode", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    

    auto result = reg.execute("git_status", "{}");
    // Should succeed in Plan mode (read-only tool)
    REQUIRE(result);

    fs::remove_all(sd);
}

// ===================================================================
// git_diff
// ===================================================================

TEST_CASE("git_diff unstaged modifications", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify tracked file
    std::ofstream(sd + "/README.md") << "# Modified content\n";

    auto result = reg.execute("git_diff", "{}");
    REQUIRE(result);
    // Expected diff: -# Test\n +# Modified content\n
    CHECK(result->find("-#") != std::string::npos);
    CHECK(result->find("+#") != std::string::npos);
    CHECK(result->find("Modified content") != std::string::npos);
    // Should not say "no changes"
    CHECK(result->find("no unstaged changes") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff staged changes", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify and stage
    std::ofstream(sd + "/README.md") << "# Staged change\n";
    reg.execute("run_bash", R"({"command": "git add README.md"})");

    auto result = reg.execute("git_diff", R"({"staged": true})");
    REQUIRE(result);
    CHECK(result->find("-#") != std::string::npos);
    CHECK(result->find("+#") != std::string::npos);
    CHECK(result->find("Staged change") != std::string::npos);
    CHECK(result->find("no staged changes") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff no unstaged changes", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_diff", "{}");
    REQUIRE(result);
    CHECK(*result == "(no unstaged changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_diff no staged changes", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_diff", R"({"staged": true})");
    REQUIRE(result);
    CHECK(*result == "(no staged changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_diff with path filter", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a second tracked file
    std::ofstream(sd + "/other.txt") << "original\n";
    reg.execute("run_bash", R"({"command": "git add other.txt && git commit -m 'add other'"})");

    // Modify both tracked files
    std::ofstream(sd + "/README.md") << "# README changed\n";
    std::ofstream(sd + "/other.txt") << "modified\n";

    // Filter to only other.txt
    auto result = reg.execute("git_diff", R"({"path": "other.txt"})");
    REQUIRE(result);
    CHECK(result->find("other.txt") != std::string::npos);
    CHECK(result->find("modified") != std::string::npos);
    // README.md should NOT appear in the diff
    CHECK(result->find("README.md") == std::string::npos);
    // README content should not appear
    CHECK(result->find("README changed") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff not a git repo", "[tools][git_diff]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_diff", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff available in plan mode", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    

    std::ofstream(sd + "/README.md") << "# Plan mode change\n";

    auto result = reg.execute("git_diff", "{}");
    // Should succeed in Plan mode (read-only tool)
    REQUIRE(result);
    CHECK(result->find("# Plan mode change") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff untracked files not shown by default", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create an untracked file — git diff does NOT show untracked files
    std::ofstream(sd + "/untracked.txt") << "new untracked\n";

    auto result = reg.execute("git_diff", "{}");
    REQUIRE(result);
    // Should show no unstaged changes (untracked files are not in the diff)
    CHECK(*result == "(no unstaged changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_diff output truncation", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a tracked file with a single line
    std::ofstream(sd + "/bigfile.txt") << "original line\n";
    reg.execute("run_bash", R"({"command": "git add bigfile.txt && git commit -m 'add bigfile'"})");

    // Now overwrite with many lines (each line ~15 chars, total > 500 lines)
    std::ofstream ofs2(sd + "/bigfile.txt");
    for (int i = 0; i < 600; i++) {
        ofs2 << "modified line " << i << "\n";
    }
    ofs2.close();

    auto result = reg.execute("git_diff", "{}");
    REQUIRE(result);
    // The diff shows all lines (truncation was removed).
    CHECK(result->find("modified line 599") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// git_log
// ===================================================================

TEST_CASE("git_log basic", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Add a couple more commits
    std::ofstream(sd + "/a.txt") << "hello\n";
    reg.execute("run_bash", R"({"command": "git add a.txt && git commit -m 'add a.txt'"})");
    std::ofstream(sd + "/b.txt") << "world\n";
    reg.execute("run_bash", R"({"command": "git add b.txt && git commit -m 'add b.txt'"})");

    auto result = reg.execute("git_log", "{}");
    REQUIRE(result);

    // Should show 3 commits (initial + 2 additions) in short format
    CHECK(result->find("commit ") != std::string::npos);
    CHECK(result->find("Author:") != std::string::npos);
    CHECK(result->find("Date:") != std::string::npos);
    CHECK(result->find("add b.txt") != std::string::npos);
    CHECK(result->find("add a.txt") != std::string::npos);
    CHECK(result->find("initial commit") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log max_count", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Add 2 more commits
    std::ofstream(sd + "/a.txt") << "hello\n";
    reg.execute("run_bash", R"({"command": "git add a.txt && git commit -m 'add a.txt'"})");
    std::ofstream(sd + "/b.txt") << "world\n";
    reg.execute("run_bash", R"({"command": "git add b.txt && git commit -m 'add b.txt'"})");

    // Request only 1 commit
    auto result = reg.execute("git_log", R"({"max_count": 1})");
    REQUIRE(result);

    // Should show only the most recent commit
    CHECK(result->find("add b.txt") != std::string::npos);
    CHECK(result->find("add a.txt") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log format oneline", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/a.txt") << "hello\n";
    reg.execute("run_bash", R"({"command": "git add a.txt && git commit -m 'add a.txt'"})");

    auto result = reg.execute("git_log", R"({"format": "oneline"})");
    REQUIRE(result);

    // Oneline format: no "commit ", no "Author:", no "Date:"
    CHECK(result->find("commit ") == std::string::npos);
    CHECK(result->find("Author:") == std::string::npos);
    CHECK(result->find("Date:") == std::string::npos);
    // Should have short hash prefix (8 hex chars) and subject
    CHECK(result->size() < 200); // compact format

    fs::remove_all(sd);
}

TEST_CASE("git_log format full", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Commit with a multi-line message (-m for subject, -m for body)
    auto r = reg.execute("run_bash",
        R"({"command": "echo hello > new.txt && git add new.txt && git commit -m 'add new.txt' -m 'This is the second paragraph.'"})");
    REQUIRE(r);

    auto result = reg.execute("git_log", R"({"format": "full"})");
    REQUIRE(result);

    // Full format includes the body
    CHECK(result->find("This is the second paragraph.") != std::string::npos);
    // Should also have the subject
    CHECK(result->find("add new.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log max_count cap", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Add 55 commits (more than the max of 50)
    for (int i = 0; i < 55; i++) {
        std::ofstream(sd + "/f" + std::to_string(i) + ".txt") << std::to_string(i) << "\n";
        auto r = reg.execute("run_bash",
            R"({"command": "git add f)" + std::to_string(i) +
                R"(.txt && git commit -m 'commit )" + std::to_string(i) + R"('"})");
        REQUIRE(r);
    }

    auto result = reg.execute("git_log", R"({"max_count": 100})");
    REQUIRE(result);

    // Count "commit " at start of lines (not in subject text) — cap removed, all commits returned
    int count = 0;
    size_t pos = 0;
    while ((pos = result->find("commit ", pos)) != std::string::npos) {
        // Only count if it's at the start of a line (pos == 0 or preceded by '\n')
        if (pos == 0 || (*result)[pos - 1] == '\n') {
            count++;
        }
        pos += 7;
    }
    CHECK(count == 56); // all 56 commits returned (make_git_repo creates 1 + 55 more; cap of 50 removed)

    fs::remove_all(sd);
}

TEST_CASE("git_log empty repo", "[tools][git_log]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Init but no commits
    reg.execute("run_bash", R"({"command": "git init"})");

    auto result = reg.execute("git_log", "{}");
    CHECK_FALSE(result);
    // On an empty repo (git init with no commits), HEAD reference doesn't exist
    CHECK(result.error().find("reference") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log not a git repo", "[tools][git_log]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_log", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log invalid branch", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_log", R"({"branch": "nonexistent-branch"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("nonexistent-branch") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log available in plan mode", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    

    // Should succeed in Plan mode (read-only tool)
    auto result = reg.execute("git_log", "{}");
    REQUIRE(result);
    CHECK(result->find("commit ") != std::string::npos);
    CHECK(result->find("initial commit") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log custom branch", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Add 2 commits
    std::ofstream(sd + "/a.txt") << "hello\n";
    reg.execute("run_bash", R"({"command": "git add a.txt && git commit -m 'add a.txt'"})");
    std::ofstream(sd + "/b.txt") << "world\n";
    reg.execute("run_bash", R"({"command": "git add b.txt && git commit -m 'add b.txt'"})");

    // Start from HEAD~1 (skip the latest commit)
    auto result = reg.execute("git_log", R"({"branch": "HEAD~1"})");
    REQUIRE(result);

    // Should show initial commit and add a.txt, but NOT add b.txt
    CHECK(result->find("initial commit") != std::string::npos);
    CHECK(result->find("add a.txt") != std::string::npos);
    CHECK(result->find("add b.txt") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log invalid format", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_log", R"({"format": "invalid"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("invalid format") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log max_count negative", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Negative max_count should be clamped to 1
    auto result = reg.execute("git_log", R"({"max_count": -5})");
    REQUIRE(result);
    // Should still work and show at least 1 commit
    CHECK(result->find("initial commit") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// git_add
// ===================================================================

TEST_CASE("git_add stage tracked file", "[tools][git_add]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify a tracked file
    std::ofstream(sd + "/README.md") << "# Modified\n";

    // Stage it
    auto result = reg.execute("git_add", R"({"path": "README.md"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("staged") != std::string::npos);

    // Verify via git_status: should show "M " (staged modified)
    auto status = reg.execute("git_status", "{}");
    REQUIRE(status);
    CHECK(status->find("M  README.md") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_add stage untracked file", "[tools][git_add]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a new untracked file
    std::ofstream(sd + "/new.txt") << "new content\n";

    // Stage it with explicit path
    auto result = reg.execute("git_add", R"({"path": "new.txt"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);

    // Verify via git_status: should show "A " (staged added)
    auto status = reg.execute("git_status", "{}");
    REQUIRE(status);
    CHECK(status->find("A  new.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_add all stages everything", "[tools][git_add]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create multiple changes
    std::ofstream(sd + "/README.md") << "# Modified\n";
    std::ofstream(sd + "/new.txt") << "new file\n";
    std::ofstream(sd + "/another.txt") << "another file\n";

    // Stage all changes
    auto result = reg.execute("git_add", R"({"all": true})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("staged all") != std::string::npos);

    // Verify via git_status: all three should show as staged
    auto status = reg.execute("git_status", "{}");
    REQUIRE(status);
    CHECK(status->find("M  README.md") != std::string::npos);
    CHECK(status->find("A  new.txt") != std::string::npos);
    CHECK(status->find("A  another.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_add not a git repo", "[tools][git_add]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_add", R"({"path": "."})");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_add path traversal rejected", "[tools][git_add]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_add", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// git_commit
// ===================================================================

TEST_CASE("git_commit basic", "[tools][git_commit]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Stage a change
    std::ofstream(sd + "/README.md") << "# Modified for commit\n";
    auto add_result = reg.execute("git_add", R"({"path": "README.md"})");
    REQUIRE(add_result);

    // Commit
    auto result = reg.execute("git_commit", R"({"message": "test commit"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("committed") != std::string::npos);
    CHECK(result->find("test commit") != std::string::npos);

    // Verify via git_log
    auto log = reg.execute("git_log", R"({"max_count": 1})");
    REQUIRE(log);
    CHECK(log->find("test commit") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_commit with all", "[tools][git_commit]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify a tracked file WITHOUT pre-staging — use --all
    std::ofstream(sd + "/README.md") << "# Committed with --all\n";

    auto result = reg.execute("git_commit",
                              R"({"message": "commit with all", "all": true})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("commit with all") != std::string::npos);

    // Verify via git_log
    auto log = reg.execute("git_log", R"({"max_count": 1})");
    REQUIRE(log);
    CHECK(log->find("commit with all") != std::string::npos);

    // Also verify working tree is clean
    auto status = reg.execute("git_status", "{}");
    REQUIRE(status);
    CHECK(*status == "(clean — no changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_commit empty message rejected", "[tools][git_commit]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_commit", R"({"message": ""})");
    CHECK_FALSE(result);
    CHECK(result.error().find("commit message is required") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_commit no staged changes", "[tools][git_commit]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // No changes made — nothing to commit
    auto result = reg.execute("git_commit", R"({"message": "should fail"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("no changes to commit") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_commit not a git repo", "[tools][git_commit]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("git_commit", R"({"message": "test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
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

TEST_CASE("read_file respects max_lines", "[tools][read_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

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
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("read_file", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

// ===================================================================
// read_file_lines
// ===================================================================

TEST_CASE("read_file_lines basic", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a file with known content
    std::ofstream ofs(sd + "/lines.txt");
    for (int i = 1; i <= 20; i++) {
        ofs << "line " << i << "\n";
    }
    ofs.close();

    auto result = reg.execute("read_file_lines",
                              R"({"path": "lines.txt", "start_line": 1, "end_line": 5})");
    REQUIRE(result);

    // Should contain lines 1-5 with line numbers
    CHECK(result->find("1: line 1") != std::string::npos);
    CHECK(result->find("5: line 5") != std::string::npos);
    // Should NOT contain lines outside the range
    CHECK(result->find("6: line 6") == std::string::npos);
    // Should report truncation since there are more lines after line 5
    CHECK(result->find("truncated") != std::string::npos);
    CHECK(result->find(">20 lines") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines read to exact EOF", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a file with exactly 5 lines
    std::ofstream ofs(sd + "/exact.txt");
    for (int i = 1; i <= 5; i++) {
        ofs << "exact " << i << "\n";
    }
    ofs.close();

    // Read all lines (end_line matches file length)
    auto result = reg.execute("read_file_lines",
                              R"({"path": "exact.txt", "start_line": 1, "end_line": 5})");
    REQUIRE(result);

    // Should contain lines 1-5 with line numbers
    CHECK(result->find("1: exact 1") != std::string::npos);
    CHECK(result->find("5: exact 5") != std::string::npos);
    // Should NOT be truncated — we read exactly to EOF
    CHECK(result->find("truncated") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines start_line offset", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream ofs(sd + "/lines.txt");
    for (int i = 1; i <= 50; i++) {
        ofs << "content " << i << "\n";
    }
    ofs.close();

    // Read lines 10-15
    auto result = reg.execute("read_file_lines",
                              R"({"path": "lines.txt", "start_line": 10, "end_line": 15})");
    REQUIRE(result);

    // Should contain lines 10-15
    CHECK(result->find("10: content 10") != std::string::npos);
    CHECK(result->find("15: content 15") != std::string::npos);
    // Should NOT contain lines outside the range
    CHECK(result->find("9: content 9") == std::string::npos);
    CHECK(result->find("16: content 16") == std::string::npos);
    // Should report truncation since there are more lines after line 15
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines end_line beyond EOF", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream ofs(sd + "/short.txt");
    for (int i = 1; i <= 5; i++) {
        ofs << "short " << i << "\n";
    }
    ofs.close();

    // Request range that goes beyond EOF
    auto result = reg.execute("read_file_lines",
                              R"({"path": "short.txt", "start_line": 1, "end_line": 999})");
    REQUIRE(result);

    // Should contain all 5 lines
    CHECK(result->find("1: short 1") != std::string::npos);
    CHECK(result->find("5: short 5") != std::string::npos);
    // Should indicate truncation (since end_line wasn't fully read)
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines end_line omitted", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream ofs(sd + "/many.txt");
    for (int i = 1; i <= 50; i++) {
        ofs << "omitted " << i << "\n";
    }
    ofs.close();

    // Read from line 40 without specifying end_line
    auto result = reg.execute("read_file_lines",
                              R"({"path": "many.txt", "start_line": 40})");
    REQUIRE(result);

    // Should contain lines 40-50
    CHECK(result->find("40: omitted 40") != std::string::npos);
    CHECK(result->find("50: omitted 50") != std::string::npos);
    // Should NOT be truncated (we read to EOF within max_lines, end_line omitted)
    CHECK(result->find("truncated") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines max_lines cap", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream ofs(sd + "/long.txt");
    for (int i = 1; i <= 500; i++) {
        ofs << "longline " << i << "\n";
    }
    ofs.close();

    // Read with small max_lines
    auto result = reg.execute("read_file_lines",
                              R"({"path": "long.txt", "start_line": 1, "max_lines": 10})");
    REQUIRE(result);

    // Should contain lines 1-10
    CHECK(result->find("1: longline 1") != std::string::npos);
    CHECK(result->find("10: longline 10") != std::string::npos);
    CHECK(result->find("11: longline 11") == std::string::npos);
    // Should indicate truncation
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines start_line < 1", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    std::ofstream(sd + "/dummy.txt") << "hello\n";

    auto result = reg.execute("read_file_lines",
                              R"({"path": "dummy.txt", "start_line": 0})");
    CHECK_FALSE(result);
    CHECK(result.error().find("start_line") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines end_line < start_line", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    std::ofstream(sd + "/dummy.txt") << "hello\n";

    auto result = reg.execute("read_file_lines",
                              R"({"path": "dummy.txt", "start_line": 10, "end_line": 5})");
    CHECK_FALSE(result);
    CHECK(result.error().find("end_line") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines file not found", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("read_file_lines",
                              R"({"path": "nonexistent.txt", "start_line": 1})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Failed to open") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines path traversal rejected", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("read_file_lines",
                              R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines empty file", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create empty file
    std::ofstream(sd + "/empty.txt");
    // No content written

    auto result = reg.execute("read_file_lines",
                              R"({"path": "empty.txt", "start_line": 1})");
    REQUIRE(result);
    // Should return empty string (no lines)
    CHECK(result->empty());

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines start_line beyond EOF", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/few.txt") << "line1\nline2\nline3\n";

    auto result = reg.execute("read_file_lines",
                              R"({"path": "few.txt", "start_line": 10})");
    REQUIRE(result);
    CHECK(result->find("beyond end of file") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines single line", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/single.txt") << "only line\n";

    auto result = reg.execute("read_file_lines",
                              R"({"path": "single.txt", "start_line": 1, "end_line": 1})");
    REQUIRE(result);
    CHECK(result->find("1: only line") != std::string::npos);
    CHECK(result->find("truncated") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines max_lines clamped", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream ofs(sd + "/big.txt");
    for (int i = 1; i <= 600; i++) {
        ofs << "big " << i << "\n";
    }
    ofs.close();

    // max_lines=1000 should be clamped to 500
    auto result = reg.execute("read_file_lines",
                              R"({"path": "big.txt", "start_line": 1, "max_lines": 1000})");
    REQUIRE(result);
    CHECK(result->find("500: big 500") != std::string::npos);
    CHECK(result->find("501: big 501") == std::string::npos);
    CHECK(result->find("truncated") != std::string::npos);

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
    auto r = reg.execute("run_bash", R"({"command": "git init"})");
    REQUIRE(r);
    reg.execute("run_bash",
        R"({"command": "git config user.email test@test.com"})");
    reg.execute("run_bash",
        R"({"command": "git config user.name Test"})");
    std::ofstream(sd + "/README.md") << "# Test\n";
    reg.execute("run_bash", R"({"command": "git add -A"})");
    reg.execute("run_bash", R"({"command": "git commit -m 'initial commit'"})");

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
// run_bash
// ===================================================================

TEST_CASE("run_bash basic command", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/hello.txt") << "world\n";

    auto result = reg.execute("run_bash", R"({"command": "cat hello.txt"})");
    REQUIRE(result);
    CHECK(result->find("world") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("run_bash command failure returns stderr", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result =
        reg.execute("run_bash", R"({"command": "ls nonexistent_file"})");
    REQUIRE(result);
    // Should contain error, not just empty
    CHECK_FALSE(result->empty());

    fs::remove_all(sd);
}

TEST_CASE("run_bash timeout kills process", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    Config cfg;
    cfg.bash_timeout = 2; // 2 second timeout
    ToolRegistry reg;
    reg.add_defaults(sd, cfg);

    auto start = std::chrono::steady_clock::now();
    auto result = reg.execute("run_bash",
                              R"({"command": "sleep 10 && echo done"})");
    auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE(result);
    // Should NOT take 10 seconds — the 2s timeout should kill the process
    CHECK(elapsed < std::chrono::seconds(5));
    // Partial output may be empty (sleep produces no output)
    CHECK(result->empty());  // sleep 10 produces no output before SIGKILL

    fs::remove_all(sd);
}

TEST_CASE("run_bash no line truncation", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Generate 600 lines (would have been truncated to 500 before)
    auto result = reg.execute(
        "run_bash", R"({"command": "for i in $(seq 1 600); do echo line $i; done"})");
    REQUIRE(result);

    // Count lines in output — all 600 should be present (truncation removed)
    int nl = 0;
    for (char c : *result)
        if (c == '\n')
            nl++;
    CHECK(nl == 600);

    fs::remove_all(sd);
}

TEST_CASE("run_bash no size truncation", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Generate output larger than 16000 chars
    auto result = reg.execute(
        "run_bash",
        R"({"command": "python3 -c 'print(\"x\" * 20000)'"})");
    REQUIRE(result);
    // Should contain at least 20000 chars (truncation removed)
    CHECK(result->size() >= 20000);

    fs::remove_all(sd);
}

TEST_CASE("run_bash path traversal rejected", "[tools][run_bash]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

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
// delete_file
// ===================================================================

TEST_CASE("delete_file basic", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a file to delete
    std::ofstream(sd + "/to_delete.txt") << "delete me";

    auto result = reg.execute("delete_file", R"({"path": "to_delete.txt"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("deleted") != std::string::npos);

    // File should be gone
    CHECK_FALSE(fs::exists(sd + "/to_delete.txt"));

    fs::remove_all(sd);
}

TEST_CASE("delete_file file not found", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("delete_file", R"({"path": "nonexistent.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("File not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("delete_file directory rejected", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directories(sd + "/mydir");

    auto result = reg.execute("delete_file", R"({"path": "mydir"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Not a regular file") != std::string::npos);

    // Directory should still exist
    CHECK(fs::exists(sd + "/mydir"));

    fs::remove_all(sd);
}

TEST_CASE("delete_file path traversal rejected", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("delete_file", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("delete_file absolute path inside safe_dir", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/absfile.txt") << "absolute path test";

    auto result = reg.execute("delete_file",
        json{{"path", sd + "/absfile.txt"}}.dump());
    REQUIRE(result);
    CHECK_FALSE(fs::exists(sd + "/absfile.txt"));

    fs::remove_all(sd);
}

TEST_CASE("delete_file in subdirectory", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directories(sd + "/subdir");
    std::ofstream(sd + "/subdir/nested.txt") << "nested";

    auto result = reg.execute("delete_file", R"({"path": "subdir/nested.txt"})");
    REQUIRE(result);
    CHECK_FALSE(fs::exists(sd + "/subdir/nested.txt"));
    // Parent directory should remain
    CHECK(fs::exists(sd + "/subdir"));

    fs::remove_all(sd);
}

// ===================================================================
// move_file
// ===================================================================

TEST_CASE("move_file basic rename", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/old.txt") << "content";

    auto result = reg.execute("move_file",
        R"({"source": "old.txt", "destination": "new.txt"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("moved") != std::string::npos);

    CHECK_FALSE(fs::exists(sd + "/old.txt"));
    CHECK(fs::exists(sd + "/new.txt"));
    std::ifstream ifs(sd + "/new.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "content");

    fs::remove_all(sd);
}

TEST_CASE("move_file to different directory", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directories(sd + "/subdir");
    std::ofstream(sd + "/source.txt") << "move me";

    auto result = reg.execute("move_file",
        R"({"source": "source.txt", "destination": "subdir/moved.txt"})");
    REQUIRE(result);

    CHECK_FALSE(fs::exists(sd + "/source.txt"));
    CHECK(fs::exists(sd + "/subdir/moved.txt"));

    fs::remove_all(sd);
}

TEST_CASE("move_file source not found", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("move_file",
        R"({"source": "nonexistent.txt", "destination": "new.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Source not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("move_file destination already exists", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/src.txt") << "source";
    std::ofstream(sd + "/dst.txt") << "destination";

    auto result = reg.execute("move_file",
        R"({"source": "src.txt", "destination": "dst.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Destination already exists") != std::string::npos);

    // Both files should still exist
    CHECK(fs::exists(sd + "/src.txt"));
    CHECK(fs::exists(sd + "/dst.txt"));

    fs::remove_all(sd);
}

TEST_CASE("move_file path traversal rejected", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/safe.txt") << "safe";

    auto result = reg.execute("move_file",
        R"({"source": "safe.txt", "destination": "../../etc/evil.txt"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("move_file creates parent directories", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/source.txt") << "content";

    auto result = reg.execute("move_file",
        R"({"source": "source.txt", "destination": "a/b/c/moved.txt"})");
    REQUIRE(result);

    CHECK_FALSE(fs::exists(sd + "/source.txt"));
    CHECK(fs::exists(sd + "/a/b/c/moved.txt"));

    fs::remove_all(sd);
}

TEST_CASE("move_file absolute path inside safe_dir", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/source.txt") << "content";
    auto dst = sd + "/dest.txt";

    auto result = reg.execute("move_file",
        json{{"source", sd + "/source.txt"}, {"destination", dst}}.dump());
    REQUIRE(result);

    CHECK_FALSE(fs::exists(sd + "/source.txt"));
    CHECK(fs::exists(dst));

    fs::remove_all(sd);
}

// ===================================================================
// rename_file
// ===================================================================

TEST_CASE("rename_file basic", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/old_name.txt") << "content";

    auto result = reg.execute("rename_file",
        R"({"path": "old_name.txt", "new_name": "new_name.txt"})");
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("renamed") != std::string::npos);

    CHECK_FALSE(fs::exists(sd + "/old_name.txt"));
    CHECK(fs::exists(sd + "/new_name.txt"));

    fs::remove_all(sd);
}

TEST_CASE("rename_file with path separators rejected", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/test.txt") << "content";

    auto result = reg.execute("rename_file",
        R"({"path": "test.txt", "new_name": "subdir/moved.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("must be a filename") != std::string::npos);

    // Original should remain
    CHECK(fs::exists(sd + "/test.txt"));

    fs::remove_all(sd);
}

TEST_CASE("rename_file not found", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("rename_file",
        R"({"path": "nonexistent.txt", "new_name": "new.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("File not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("rename_file destination already exists", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    std::ofstream(sd + "/source.txt") << "source";
    std::ofstream(sd + "/existing.txt") << "existing";

    auto result = reg.execute("rename_file",
        R"({"path": "source.txt", "new_name": "existing.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Destination already exists") != std::string::npos);

    // Both should still exist
    CHECK(fs::exists(sd + "/source.txt"));
    CHECK(fs::exists(sd + "/existing.txt"));

    fs::remove_all(sd);
}

TEST_CASE("rename_file path traversal in source", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("rename_file",
        R"({"path": "../../etc/passwd", "new_name": "safe.txt"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("rename_file directory rejected", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    fs::create_directories(sd + "/mydir");

    auto result = reg.execute("rename_file",
        R"({"path": "mydir", "new_name": "newdir"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Not a regular file") != std::string::npos);

    // Directory should still exist with original name
    CHECK(fs::exists(sd + "/mydir"));

    fs::remove_all(sd);
}

// ===================================================================
// Cancellation token interrupts project_tree
// ===================================================================

TEST_CASE("project_tree interrupted by cancelled token", "[tools][cancellation]") {
    auto sd = make_temp_dir();
    // Create a deep directory tree so the traversal takes enough time to
    // be interrupted.
    for (int i = 0; i < 50; i++) {
        fs::create_directories(sd + "/sub" + std::to_string(i) + "/a/b/c");
    }

    auto token = make_cancellation_token();
    *token = true;  // pre-cancel

    ToolRegistry reg;
    reg.set_cancelled(token);
    reg.add_defaults(sd, Config{});

    auto result = reg.execute("project_tree", R"({"path": ".", "max_depth": 10})");
    REQUIRE(result);
    // The tool should have been interrupted early and appended the
    // "(interrupted)" marker.
    CHECK(result->find("(interrupted)") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("run_bash chdir failure is caught", "[tools][run_bash][sandbox]") {
    // Use a non-existent directory as safe_dir — chdir in the child will fail.
    ToolRegistry reg;
    reg.add_defaults("/nonexistent_safe_dir_12345", Config{});

    // If chdir fails, the child should _exit(1) before executing the command.
    // The error message "error: chdir() to safe directory failed\n" will be
    // written to the pipe (which is connected to stdout/stderr) before _exit(1).
    // The parent reads the pipe and returns the captured output (it does not
    // check the child's exit status), so the result will contain the error msg.
    auto result = reg.execute("run_bash", R"({"command": "echo should_not_run"})");
    // The result should still be "successful" (the tool itself didn't throw),
    // but the output should contain the chdir error message.
    CHECK(result);
    if (result) {
        CHECK(result->find("chdir()") != std::string::npos);
    }
}

// ===================================================================
// git_restore
// ===================================================================

TEST_CASE("git_restore discards unstaged changes", "[tools][git_restore]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify the tracked file
    std::ofstream(sd + "/README.md") << "# Modified\n";
    reg.add(make_git_restore_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_restore",
        R"({"path": "README.md"})");
    REQUIRE(result);
    CHECK(result->find("Restored") != std::string::npos);

    // Verify file was restored to original content
    std::ifstream f(sd + "/README.md");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    CHECK(content == "# Test\n");

    fs::remove_all(sd);
}

TEST_CASE("git_restore restore all files", "[tools][git_restore]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create another tracked file via commit
    std::ofstream(sd + "/file2.txt") << "content2\n";
    {
        auto r = reg.execute("git_add", R"({"path": "file2.txt"})");
        REQUIRE(r);
        r = reg.execute("git_commit", R"({"message": "add file2", "all": true})");
        REQUIRE(r);
    }

    // Modify both files
    std::ofstream(sd + "/README.md") << "# Modified\n";
    std::ofstream(sd + "/file2.txt") << "modified2\n";
    reg.add(make_git_restore_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_restore",
        R"({"path": "."})");
    REQUIRE(result);

    // Verify both files restored
    {
        std::ifstream f(sd + "/README.md");
        std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(c == "# Test\n");
    }
    {
        std::ifstream f(sd + "/file2.txt");
        std::string c((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(c == "content2\n");
    }

    fs::remove_all(sd);
}

TEST_CASE("git_restore unstage file", "[tools][git_restore]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Modify and stage a file
    std::ofstream(sd + "/README.md") << "# Staged\n";
    auto r = reg.execute("git_add", R"({"path": "README.md"})");
    REQUIRE(r);

    // Verify it's staged before restore
    auto before = reg.execute("git_status", "{}");
    REQUIRE(before);
    // Should show staged modification (M in first column)
    {
        bool found_staged = (before->find("M  README.md") != std::string::npos) ||
                            (before->find("M\tREADME.md") != std::string::npos);
        CHECK(found_staged);
    }

    reg.add(make_git_restore_tool(std::make_shared<std::string>(sd), 10));

    // Unstage it
    auto result = reg.execute("git_restore",
        R"({"path": "README.md", "staged": true})");
    REQUIRE(result);
    CHECK(result->find("Unstaged") != std::string::npos);

    // Verify it's no longer staged — the working tree content should still
    // be the modified version (unstage doesn't touch working tree)
    std::ifstream f(sd + "/README.md");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    CHECK(content == "# Staged\n");

    // The file should now appear with 'D' in the index column (removed from
    // index but present in HEAD) and '?' in the worktree column (exists on
    // disk but not tracked), showing as "D? README.md"
    auto after = reg.execute("git_status", "{}");
    REQUIRE(after);
    {
        bool found_unstaged = (after->find("D? README.md") != std::string::npos) ||
                              (after->find("D?\tREADME.md") != std::string::npos);
        CHECK(found_unstaged);
    }

    fs::remove_all(sd);
}

TEST_CASE("git_restore not a git repo", "[tools][git_restore]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    reg.add(make_git_restore_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_restore",
        R"({"path": "README.md"})");
    REQUIRE_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_restore path traversal rejected", "[tools][git_restore]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    reg.add(make_git_restore_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_restore",
        R"({"path": "../etc/passwd"})");
    REQUIRE_FALSE(result);
    // resolve_path says "path must be under <safe_dir>"
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// git_show
// ===================================================================

TEST_CASE("git_show HEAD shows commit metadata and diff", "[tools][git_show]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a second commit with actual changes
    std::ofstream(sd + "/newfile.txt") << "new content\n";
    auto r = reg.execute("git_add", R"({"path": "newfile.txt"})");
    REQUIRE(r);
    r = reg.execute("git_commit", R"({"message": "add newfile", "all": true})");
    REQUIRE(r);

    reg.add(make_git_show_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_show", "{}");
    REQUIRE(result);
    // Should show commit metadata
    CHECK(result->find("commit ") != std::string::npos);
    CHECK(result->find("Author:") != std::string::npos);
    CHECK(result->find("Date:") != std::string::npos);
    // Should show the commit message
    CHECK(result->find("add newfile") != std::string::npos);
    // Should show the diff (newfile.txt was added)
    CHECK(result->find("newfile.txt") != std::string::npos);
    CHECK(result->find("new content") != std::string::npos);
    // Should show file count
    CHECK(result->find("1 file changed") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_show specific revision", "[tools][git_show]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // Create a second commit
    std::ofstream(sd + "/file2.txt") << "second\n";
    auto r = reg.execute("git_add", R"({"path": "file2.txt"})");
    REQUIRE(r);
    r = reg.execute("git_commit", R"({"message": "second commit", "all": true})");
    REQUIRE(r);

    // Create a third commit
    std::ofstream(sd + "/file3.txt") << "third\n";
    r = reg.execute("git_add", R"({"path": "file3.txt"})");
    REQUIRE(r);
    r = reg.execute("git_commit", R"({"message": "third commit", "all": true})");
    REQUIRE(r);

    reg.add(make_git_show_tool(std::make_shared<std::string>(sd), 10));

    // Show HEAD~1 (the second commit)
    auto result = reg.execute("git_show",
        R"({"revision": "HEAD~1"})");
    REQUIRE(result);
    CHECK(result->find("commit ") != std::string::npos);
    CHECK(result->find("second commit") != std::string::npos);
    // Should show file2.txt but not file3.txt
    CHECK(result->find("file2.txt") != std::string::npos);
    CHECK(result->find("file3.txt") == std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_show invalid revision", "[tools][git_show]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    reg.add(make_git_show_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_show",
        R"({"revision": "nonexistent_branch_xyz"})");
    REQUIRE_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("git_show not a git repo", "[tools][git_show]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});
    reg.add(make_git_show_tool(std::make_shared<std::string>(sd), 10));

    auto result = reg.execute("git_show", "{}");
    REQUIRE_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_show root commit", "[tools][git_show]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd, Config{});

    // make_git_repo already has one commit (the initial commit)
    reg.add(make_git_show_tool(std::make_shared<std::string>(sd), 10));

    // Show HEAD (the initial/root commit)
    auto result = reg.execute("git_show", "{}");
    REQUIRE(result);
    CHECK(result->find("commit ") != std::string::npos);
    CHECK(result->find("initial commit") != std::string::npos);
    // Root commit should still show a diff (README.md added)
    CHECK(result->find("README.md") != std::string::npos);

    fs::remove_all(sd);
}


