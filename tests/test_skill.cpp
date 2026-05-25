#include "skill.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ===================================================================
// Helpers
// ===================================================================

/// Create a temporary directory and return its path.
static std::string make_temp_dir() {
    char tmpl[] = "/tmp/cima_test_skills_XXXXXX";
    char* result = mkdtemp(tmpl);
    REQUIRE(result != nullptr);
    return result;
}

/// Write a SKILL.md file into a skill subdirectory under base_dir.
/// @param base_dir  Parent directory (e.g. the fake ~/.agents/skills/)
/// @param skill_name  Name of the skill (subdirectory and frontmatter name)
/// @param description  Description for frontmatter
/// @param body  Markdown body (can be empty)
static void write_skill(const std::string& base_dir,
    const std::string& skill_name,
    const std::string& description,
    const std::string& body) {
    auto skill_dir = fs::path(base_dir) / skill_name;
    fs::create_directories(skill_dir);

    std::ofstream f(skill_dir / "SKILL.md");
    REQUIRE(f.is_open());
    f << "---\n";
    f << "name: " << skill_name << "\n";
    f << "description: " << description << "\n";
    f << "---\n";
    if (!body.empty())
        f << "\n" << body << "\n";
    f.close();
}

/// Override HOME for the duration of a test.
/// Saves the old HOME value and restores it when the guard is destroyed.
struct HomeGuard {
    std::string old_home;
    std::string new_home;

    HomeGuard(const std::string& fake_home) : new_home(fake_home) {
        const char* old = std::getenv("HOME");
        old_home = old ? old : "";
        int r = setenv("HOME", fake_home.c_str(), 1);
        REQUIRE(r == 0);
    }

    ~HomeGuard() {
        if (old_home.empty())
            unsetenv("HOME");
        else
            setenv("HOME", old_home.c_str(), 1);
    }
};

// ===================================================================
// Tests: YAML frontmatter parsing
// ===================================================================

TEST_CASE("Parse valid SKILL.md", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    write_skill(skills_dir.string(), "my-skill", "Does something useful.",
        "## Usage\n\nCall this skill when you need to do something.");

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    REQUIRE(registry.size() == 1);
    const Skill* s = registry.find("my-skill");
    REQUIRE(s != nullptr);
    CHECK(s->name == "my-skill");
    CHECK(s->description == "Does something useful.");
    CHECK(s->body.find("## Usage") != std::string::npos);
    CHECK(s->body.find("Call this skill when you need to do something.") != std::string::npos);

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with missing name", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "bad-skill";
    fs::create_directories(skill_dir);

    // Write a SKILL.md that's missing the name field
    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "---\n";
        f << "description: orphan skill\n";
        f << "---\n";
        f << "\nSome body\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    // Should be skipped (fail-open)
    CHECK(registry.size() == 0);

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with missing description", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "bad-skill";
    fs::create_directories(skill_dir);

    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "---\n";
        f << "name: nameless\n";
        f << "---\n";
        f << "\nSome body\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    CHECK(registry.size() == 0);

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with no frontmatter", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "bare";
    fs::create_directories(skill_dir);

    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "Just a plain markdown file, no frontmatter.\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    CHECK(registry.size() == 0);

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with extra frontmatter fields", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "expert";
    fs::create_directories(skill_dir);

    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "---\n";
        f << "name: extra-fields\n";
        f << "description: Has extra metadata\n";
        f << "version: 1.2.0\n";
        f << "author: Test Author\n";
        f << "tags:\n";
        f << "  - tag1\n";
        f << "  - tag2\n";
        f << "---\n";
        f << "\nBody content here.\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    REQUIRE(registry.size() == 1);
    const Skill* s = registry.find("extra-fields");
    REQUIRE(s != nullptr);
    CHECK(s->name == "extra-fields");
    CHECK(s->description == "Has extra metadata");

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with quoted description", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "quoted";
    fs::create_directories(skill_dir);

    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "---\n";
        f << "name: quoted-skill\n";
        f << "description: \"A description with spaces and: colons\"\n";
        f << "---\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    REQUIRE(registry.size() == 1);
    const Skill* s = registry.find("quoted-skill");
    REQUIRE(s != nullptr);
    CHECK(s->description == "A description with spaces and: colons");

    fs::remove_all(tmp);
}

TEST_CASE("Parse SKILL.md with single-quoted description", "[skill][parse]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";
    auto skill_dir = skills_dir / "singleq";
    fs::create_directories(skill_dir);

    {
        std::ofstream f(skill_dir / "SKILL.md");
        f << "---\n";
        f << "name: singleq\n";
        f << "description: 'Single quoted value'\n";
        f << "---\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    REQUIRE(registry.size() == 1);
    const Skill* s = registry.find("singleq");
    REQUIRE(s != nullptr);
    CHECK(s->description == "Single quoted value");

    fs::remove_all(tmp);
}

// ===================================================================
// Tests: Registry scanning behavior
// ===================================================================

TEST_CASE("SkillRegistry discovers multiple skills", "[skill][registry]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";

    write_skill(skills_dir.string(), "alpha", "First skill", "Alpha body");
    write_skill(skills_dir.string(), "beta", "Second skill", "Beta body");
    write_skill(skills_dir.string(), "gamma", "Third skill", "Gamma body");

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    CHECK(registry.size() == 3);
    CHECK(registry.find("alpha") != nullptr);
    CHECK(registry.find("beta") != nullptr);
    CHECK(registry.find("gamma") != nullptr);
    CHECK(registry.find("nonexistent") == nullptr);

    fs::remove_all(tmp);
}

TEST_CASE("SkillRegistry skips directories without SKILL.md", "[skill][registry]") {
    auto tmp = make_temp_dir();
    auto skills_dir = fs::path(tmp) / ".agents" / "skills";

    // A valid skill
    write_skill(skills_dir.string(), "valid", "A valid skill", "OK");

    // A directory with no SKILL.md
    fs::create_directories(skills_dir / "empty-dir");

    // A file (not a directory) in skills dir
    {
        std::ofstream f(skills_dir / "not-a-dir.txt");
        f << "just a file\n";
    }

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    REQUIRE(registry.size() == 1);
    CHECK(registry.find("valid") != nullptr);

    fs::remove_all(tmp);
}

TEST_CASE("SkillRegistry handles missing ~/.agents/skills/ directory",
    "[skill][registry]") {
    // Use a HOME that doesn't have .agents/skills
    auto tmp = make_temp_dir();
    // Don't create .agents/skills

    HomeGuard guard(tmp);
    SkillRegistry registry;
    registry.scan();

    CHECK(registry.size() == 0);
    CHECK(registry.find("anything") == nullptr);

    fs::remove_all(tmp);
}

TEST_CASE("SkillRegistry handles empty HOME", "[skill][registry]") {
    // Temporarily unset HOME
    const char* old_home = std::getenv("HOME");
    unsetenv("HOME");

    SkillRegistry registry;
    registry.scan();
    CHECK(registry.size() == 0);

    // Restore HOME
    if (old_home)
        setenv("HOME", old_home, 1);
}
