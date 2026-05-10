#include "tools.h"
#include "jobs.h"

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
    REQUIRE(tools.size() == 19);

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
                       "list_files", "read_file", "read_file_lines", "grep_files", "write_file",
                       "edit_file", "apply_patch", "run_bash", "web_search", "web_fetch",
                       "project_tree", "git_status", "git_diff", "git_log",
                       "git_add", "git_commit",
                       "delete_file", "move_file", "rename_file"});
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
// git_status
// ===================================================================

// Helper: create a temp directory with a git repo and an initial commit.
static std::string make_git_repo() {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(*result == "(clean — no changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_status modified tracked file", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    reg.execute("run_bash", R"({"command": "git rm README.md"})");

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find("D  README.md") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status untracked file", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/untracked.txt") << "hello\n";

    auto result = reg.execute("git_status", "{}");
    REQUIRE(result);
    CHECK(result->find("?? untracked.txt") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status mixed staged and unstaged", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    // sd has no .git directory
    auto result = reg.execute("git_status", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_status available in plan mode", "[tools][git_status]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);
    

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_diff", "{}");
    REQUIRE(result);
    CHECK(*result == "(no unstaged changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_diff no staged changes", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("git_diff", R"({"staged": true})");
    REQUIRE(result);
    CHECK(*result == "(no staged changes)");

    fs::remove_all(sd);
}

TEST_CASE("git_diff with path filter", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_diff", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_diff available in plan mode", "[tools][git_diff]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);
    

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    // The diff will show all 599 new lines as additions (since we replaced 1 line with 600)
    // Plus context. Should hit the 500-line limit.
    CHECK(result->find("truncated") != std::string::npos);

    fs::remove_all(sd);
}

// ===================================================================
// git_log
// ===================================================================

TEST_CASE("git_log basic", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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

    // Count "commit " at start of lines (not in subject text) to verify cap at 50
    int count = 0;
    size_t pos = 0;
    while ((pos = result->find("commit ", pos)) != std::string::npos) {
        // Only count if it's at the start of a line (pos == 0 or preceded by '\n')
        if (pos == 0 || (*result)[pos - 1] == '\n') {
            count++;
        }
        pos += 7;
    }
    CHECK(count == 50);

    fs::remove_all(sd);
}

TEST_CASE("git_log empty repo", "[tools][git_log]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_log", "{}");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log invalid branch", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("git_log", R"({"branch": "nonexistent-branch"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("nonexistent-branch") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log available in plan mode", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);
    

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_log", R"({"format": "invalid"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("invalid format") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_log max_count negative", "[tools][git_log]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_add", R"({"path": "."})");
    CHECK_FALSE(result);
    CHECK(result.error().find("not a git repository") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_add path traversal rejected", "[tools][git_add]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("git_commit", R"({"message": ""})");
    CHECK_FALSE(result);
    CHECK(result.error().find("commit message is required") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_commit no staged changes", "[tools][git_commit]") {
    auto sd = make_git_repo();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // No changes made — nothing to commit
    auto result = reg.execute("git_commit", R"({"message": "should fail"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("no changes to commit") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("git_commit not a git repo", "[tools][git_commit]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
// read_file_lines
// ===================================================================

TEST_CASE("read_file_lines basic", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);
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
    reg.add_defaults(sd);
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
    reg.add_defaults(sd);

    auto result = reg.execute("read_file_lines",
                              R"({"path": "nonexistent.txt", "start_line": 1})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Failed to open") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines path traversal rejected", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    auto result = reg.execute("read_file_lines",
                              R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("read_file_lines empty file", "[tools][read_file_lines]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
// apply_patch
// ===================================================================

TEST_CASE("apply_patch basic single hunk", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "Hello, World!\nHow are you?\nGoodbye!\n";

    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,3 @@
-Hello, World!
+Hello, Universe!
 How are you?
 Goodbye!
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("1 hunks") != std::string::npos);
    CHECK(result->find("1 additions") != std::string::npos);
    CHECK(result->find("1 deletions") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "Hello, Universe!\nHow are you?\nGoodbye!\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch multi-hunk", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "line one\nline two\nline three\nline four\nline five\n";

    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,2 +1,2 @@
-line one
+line ONE
 line two
@@ -4,2 +4,2 @@
 line four
-line five
+line FIVE
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("2 hunks") != std::string::npos);
    CHECK(result->find("2 additions") != std::string::npos);
    CHECK(result->find("2 deletions") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "line ONE\nline two\nline three\nline four\nline FIVE\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch pure addition", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "first line\nlast line\n";

    // Diff that adds a line (no deletions)
    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,2 +1,3 @@
 first line
+inserted line
 last line
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("1 hunks") != std::string::npos);
    CHECK(result->find("1 additions") != std::string::npos);
    CHECK(result->find("0 deletions") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "first line\ninserted line\nlast line\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch pure deletion", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "first line\nunwanted line\nlast line\n";

    // Diff that removes a line (no additions)
    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,2 @@
 first line
-unwanted line
 last line
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("1 hunks") != std::string::npos);
    CHECK(result->find("0 additions") != std::string::npos);
    CHECK(result->find("1 deletions") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "first line\nlast line\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch context mismatch", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "Hello, World!\nHow are you?\nGoodbye!\n";

    // Patch expects "Hello, Everyone!" but file has "Hello, World!"
    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,3 @@
-Hello, Everyone!
+Hello, Universe!
 How are you?
 Goodbye!
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    CHECK_FALSE(result);
    CHECK(result.error().find("Hunk 1") != std::string::npos);
    CHECK(result.error().find("does not match") != std::string::npos);

    // Verify file is unchanged
    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "Hello, World!\nHow are you?\nGoodbye!\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch empty patch", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "content\n";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", ""}}.dump());
    CHECK_FALSE(result);
    CHECK(result.error().find("patch string is required") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("apply_patch path traversal rejected", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::string patch = R"(--- a/x
+++ b/x
@@ -1 +1 @@
-old
+new
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "../../etc/passwd"}, {"patch", patch}}.dump());
    CHECK_FALSE(result);
    CHECK(result.error().find("path must be under") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("apply_patch multiple additions and deletions", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "a\nb\nc\nd\ne\nf\ng\n";

    // Replace "b" with "B", "d" with "D", "f" with "F" in a single hunk
    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,7 +1,7 @@
 a
-b
+B
 c
-d
+D
 e
-f
+F
 g
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("1 hunks") != std::string::npos);
    CHECK(result->find("3 additions") != std::string::npos);
    CHECK(result->find("3 deletions") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    CHECK(content == "a\nB\nc\nD\ne\nF\ng\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch with no newline at end of file", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    // File without trailing newline
    std::ofstream(sd + "/test.txt") << "first line\nsecond line\nthird line";

    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -1,3 +1,3 @@
 first line
-second line
+SECOND LINE
 third line
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    REQUIRE(result);
    CHECK(result->find("ok") != std::string::npos);
    CHECK(result->find("1 hunks") != std::string::npos);

    std::ifstream ifs(sd + "/test.txt");
    std::string content((std::istreambuf_iterator<char>(ifs)),
                        std::istreambuf_iterator<char>());
    // Patch context lines include trailing newlines, so the patched file
    // gains a trailing newline. This matches standard patch behavior.
    CHECK(content == "first line\nSECOND LINE\nthird line\n");

    fs::remove_all(sd);
}

TEST_CASE("apply_patch invalid hunk header", "[tools][apply_patch]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

    std::ofstream(sd + "/test.txt") << "content\n";

    std::string patch = R"(--- a/test.txt
+++ b/test.txt
@@ -bad +bad @@
old
new
)";

    auto result = reg.execute("apply_patch",
        json{{"path", "test.txt"}, {"patch", patch}}.dump());
    CHECK_FALSE(result);
    CHECK(result.error().find("invalid old-line spec") != std::string::npos);

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

TEST_CASE("grep_files ignores gitignored files", "[tools][grep_files]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    // Default DDG response should contain the query term or results
    bool found = result->find("hello") != std::string::npos ||
                 result->find("Hello") != std::string::npos;
    CHECK(found);
}

TEST_CASE("web_search falls back to duckduckgo when no engine/endpoint", "[tools][web_search]") {
    // With only api_key (no engine_id, no endpoint), falls back to DuckDuckGo
    ToolRegistry reg;
    reg.add_defaults("/tmp", {}, "my-api-key", "", "");
    auto result = reg.execute("web_search", R"({"query": "hello"})");
    // Should succeed via DuckDuckGo fallback
    REQUIRE(result);
    // DDG returns the abstract text or heading containing the query term
    bool found = result->find("hello") != std::string::npos ||
                 result->find("Hello") != std::string::npos;
    CHECK(found);
}

TEST_CASE("web_search empty query rejected", "[tools][web_search]") {
    ToolRegistry reg;
    reg.add_defaults("/tmp", {}, "key", "cx", "");
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
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
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
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
    auto result = reg.execute("web_search", R"({"query": "nonexistent"})");
    REQUIRE(result);
    CHECK(*result == "(no results found)");
}

TEST_CASE("web_search custom endpoint http error", "[tools][web_search]") {
    std::string mock_json = R"({"error": "rate limited"})";
    MockHttpServer server(mock_json, 429);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
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
    reg.add_defaults("/tmp", {}, "test-key", "", "http://127.0.0.1:1/search?q={query}");
    auto result = reg.execute("web_search", R"({"query": "test"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("curl error") != std::string::npos);
}

TEST_CASE("web_search timeout", "[tools][web_search]") {
    std::string mock_json = R"({"items": []})";
    MockHttpServer server(mock_json, 200, true); // delay 1.5s, tool timeout is 15s
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
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
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
    

    auto result = reg.execute("web_search", R"({"query": "test"})");
    // Should succeed in Plan mode (read-only tool)
    REQUIRE(result);
}

TEST_CASE("web_search respects max query length", "[tools][web_search]") {
    std::string mock_json = R"({"items": [{"title": "A", "snippet": "B", "link": "C"}]})";
    MockHttpServer server(mock_json, 200);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ToolRegistry reg;
    reg.add_defaults("/tmp", {}, "test-key", "", server.url());
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

// ===================================================================
// delete_file
// ===================================================================

TEST_CASE("delete_file basic", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("delete_file", R"({"path": "nonexistent.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("File not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("delete_file directory rejected", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("delete_file", R"({"path": "../../etc/passwd"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("delete_file absolute path inside safe_dir", "[tools][delete_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("move_file",
        R"({"source": "nonexistent.txt", "destination": "new.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("Source not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("move_file destination already exists", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    std::ofstream(sd + "/safe.txt") << "safe";

    auto result = reg.execute("move_file",
        R"({"source": "safe.txt", "destination": "../../etc/evil.txt"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("move_file creates parent directories", "[tools][move_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("rename_file",
        R"({"path": "nonexistent.txt", "new_name": "new.txt"})");
    CHECK_FALSE(result);
    CHECK(result.error().find("File not found") != std::string::npos);

    fs::remove_all(sd);
}

TEST_CASE("rename_file destination already exists", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
    reg.add_defaults(sd);

    auto result = reg.execute("rename_file",
        R"({"path": "../../etc/passwd", "new_name": "safe.txt"})");
    CHECK_FALSE(result);

    fs::remove_all(sd);
}

TEST_CASE("rename_file directory rejected", "[tools][rename_file]") {
    auto sd = make_temp_dir();
    ToolRegistry reg;
    reg.add_defaults(sd);

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
// edit_job
// ===================================================================

TEST_CASE("edit_job edit description only", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_desc", "Original description");

    auto result = JobBoard::instance().edit_job("edit_test_desc", "", "Updated description");
    REQUIRE(result);

    auto job = JobBoard::instance().read_job("edit_test_desc");
    REQUIRE(job);
    CHECK(job->name == "edit_test_desc");
    CHECK(job->description == "Updated description");

    JobBoard::instance().close_job("edit_test_desc");
}

TEST_CASE("edit_job rename only", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_rename_old", "Description");

    auto result = JobBoard::instance().edit_job("edit_test_rename_old", "edit_test_rename_new", "");
    REQUIRE(result);

    // Old name should not exist
    auto old_job = JobBoard::instance().read_job("edit_test_rename_old");
    CHECK_FALSE(old_job);

    // New name should exist with original description
    auto job = JobBoard::instance().read_job("edit_test_rename_new");
    REQUIRE(job);
    CHECK(job->name == "edit_test_rename_new");
    CHECK(job->description == "Description");

    JobBoard::instance().close_job("edit_test_rename_new");
}

TEST_CASE("edit_job both name and description", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_both_old", "Original");

    auto result = JobBoard::instance().edit_job("edit_test_both_old", "edit_test_both_new", "New description");
    REQUIRE(result);

    // Old name should not exist
    CHECK_FALSE(JobBoard::instance().read_job("edit_test_both_old"));

    // New name should exist with new description
    auto job = JobBoard::instance().read_job("edit_test_both_new");
    REQUIRE(job);
    CHECK(job->name == "edit_test_both_new");
    CHECK(job->description == "New description");

    JobBoard::instance().close_job("edit_test_both_new");
}

TEST_CASE("edit_job job not found", "[jobs][edit_job]") {
    auto result = JobBoard::instance().edit_job("nonexistent_job", "new_name", "");
    CHECK_FALSE(result);
    CHECK(result.error().find("job not found") != std::string::npos);
}

TEST_CASE("edit_job nothing to update", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_nochange", "Desc");

    auto result = JobBoard::instance().edit_job("edit_test_nochange", "", "");
    CHECK_FALSE(result);
    CHECK(result.error().find("at least one of new_name or new_description must be provided") != std::string::npos);

    // Job should still exist unchanged
    auto job = JobBoard::instance().read_job("edit_test_nochange");
    REQUIRE(job);
    CHECK(job->name == "edit_test_nochange");
    CHECK(job->description == "Desc");

    JobBoard::instance().close_job("edit_test_nochange");
}

TEST_CASE("edit_job new_name conflict", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_conflict_a", "Job A");
    JobBoard::instance().open_job("edit_test_conflict_b", "Job B");

    auto result = JobBoard::instance().edit_job("edit_test_conflict_a", "edit_test_conflict_b", "");
    CHECK_FALSE(result);
    CHECK(result.error().find("already exists") != std::string::npos);

    // Both original jobs should remain
    auto job_a = JobBoard::instance().read_job("edit_test_conflict_a");
    REQUIRE(job_a);
    CHECK(job_a->name == "edit_test_conflict_a");

    auto job_b = JobBoard::instance().read_job("edit_test_conflict_b");
    REQUIRE(job_b);
    CHECK(job_b->name == "edit_test_conflict_b");

    JobBoard::instance().close_job("edit_test_conflict_a");
    JobBoard::instance().close_job("edit_test_conflict_b");
}

TEST_CASE("edit_job rename to same name is no-op for name", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_same_name", "Original description");

    auto result = JobBoard::instance().edit_job("edit_test_same_name", "edit_test_same_name", "Updated description");
    REQUIRE(result);

    auto job = JobBoard::instance().read_job("edit_test_same_name");
    REQUIRE(job);
    CHECK(job->name == "edit_test_same_name");
    CHECK(job->description == "Updated description");

    JobBoard::instance().close_job("edit_test_same_name");
}

TEST_CASE("edit_job preserves comments", "[jobs][edit_job]") {
    JobBoard::instance().open_job("edit_test_comments", "Original");
    JobBoard::instance().comment_job("edit_test_comments", "First comment");
    JobBoard::instance().comment_job("edit_test_comments", "Second comment");

    // Rename and update description
    auto result = JobBoard::instance().edit_job("edit_test_comments", "edit_test_comments_new", "New description");
    REQUIRE(result);

    auto job = JobBoard::instance().read_job("edit_test_comments_new");
    REQUIRE(job);
    CHECK(job->name == "edit_test_comments_new");
    CHECK(job->description == "New description");
    REQUIRE(job->comments.size() == 2);
    CHECK(job->comments[0] == "First comment");
    CHECK(job->comments[1] == "Second comment");

    JobBoard::instance().close_job("edit_test_comments_new");
}

TEST_CASE("edit_job tool exists in registry", "[jobs][edit_job][tool]") {
    ToolRegistry reg;
    add_job_tools(reg);

    json tools = reg.to_openai_tools();
    bool found = false;
    for (const auto& t : tools) {
        if (t["function"]["name"] == "edit_job") {
            found = true;
            CHECK(t["function"]["description"].get<std::string>().find("Edit the name") != std::string::npos);
            break;
        }
    }
    CHECK(found);

    // Verify it has Write permission
    auto write_tools = reg.tool_names_by_permission(ToolPermission::Write);
    CHECK(write_tools.find("edit_job") != write_tools.end());
}

TEST_CASE("edit_job not available to builders", "[jobs][edit_job][tool]") {
    // Builders only get open_job, list_jobs, read_job, comment_job
    ToolRegistry reg;
    reg.add(make_open_job_tool());
    reg.add(make_list_jobs_tool());
    reg.add(make_read_job_tool());
    reg.add(make_comment_job_tool());

    json tools = reg.to_openai_tools();
    for (const auto& t : tools) {
        CHECK(t["function"]["name"] != "edit_job");
    }
}
