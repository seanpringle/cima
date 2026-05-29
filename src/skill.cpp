#include "skill.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string get_home_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0])
        home = std::getenv("USERPROFILE");
    return home ? home : "";
}

/// Trim leading and trailing whitespace from a string (in-place).
static void trim(std::string& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t'))
        start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t'))
        end--;
    if (start > 0 || end < s.size())
        s = s.substr(start, end - start);
}

/// Strip surrounding quotes (single or double) from a string.
static void strip_quotes(std::string& s) {
    if (s.size() >= 2) {
        if ((s[0] == '"' && s.back() == '"') || (s[0] == '\'' && s.back() == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
}

// ---------------------------------------------------------------------------
// SkillRegistry
// ---------------------------------------------------------------------------

void SkillRegistry::scan() {
    skills_.clear();

    auto home = get_home_dir();
    if (home.empty())
        return;

    fs::path skills_dir = fs::path(home) / ".agents" / "skills";
    if (!fs::is_directory(skills_dir))
        return;

    for (const auto& entry : fs::directory_iterator(skills_dir)) {
        if (!entry.is_directory())
            continue;

        auto skill_path = entry.path() / "SKILL.md";
        if (!fs::is_regular_file(skill_path))
            continue;

        // Read the entire file
        std::ifstream file(skill_path);
        if (!file.is_open())
            continue;

        std::stringstream buf;
        buf << file.rdbuf();
        std::string content = buf.str();
        file.close();

        // ------------------------------------------------------------------
        // Parse YAML frontmatter
        // Format: ---\n<frontmatter>\n---\n<body>
        // ------------------------------------------------------------------

        // Must start with "---"
        if (content.size() < 4 || content.substr(0, 3) != "---")
            continue;
        if (content[3] != '\n' && content[3] != '\r')
            continue; // opening --- must be followed by newline

        // Find the closing ---
        size_t end_front = content.find("\n---", 3);
        if (end_front == std::string::npos)
            continue;

        // Extract frontmatter (skip opening ---)
        std::string frontmatter = content.substr(3, end_front - 3);

        // Locate body start (skip closing --- and following newline)
        size_t body_start = end_front + 4; // skip "\n---"
        if (body_start < content.size() && (content[body_start] == '\n' || content[body_start] == '\r'))
            body_start++;

        // Extract and clean body
        Skill skill;
        skill.dir = entry.path().string();
        skill.body = content.substr(body_start);

        // Trim leading newlines from body
        while (!skill.body.empty() && (skill.body[0] == '\n' || skill.body[0] == '\r'))
            skill.body.erase(0, 1);

        // Trim trailing whitespace from body
        while (
            !skill.body.empty() && (skill.body.back() == ' ' || skill.body.back() == '\t' || skill.body.back() == '\n' || skill.body.back() == '\r'))
            skill.body.pop_back();

        // Parse key-value pairs from frontmatter
        std::istringstream fm_stream(frontmatter);
        std::string line;
        bool has_name = false;
        bool has_desc = false;

        while (std::getline(fm_stream, line)) {
            // Strip carriage return
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Skip empty lines and comments
            if (line.empty() || line[0] == '#')
                continue;

            // Find the colon separator
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;

            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);

            trim(key);
            trim(value);
            strip_quotes(value);

            if (key == "name") {
                skill.name = value;
                has_name = true;
            } else if (key == "description") {
                skill.description = value;
                has_desc = true;
            }
            // Other frontmatter fields (tags, version, author, etc.) are ignored
        }

        // Fail-open: skip malformed skills without crashing the registry
        if (!has_name || !has_desc || skill.name.empty() || skill.description.empty())
            continue;

        skills_.push_back(std::move(skill));
    }
}

const Skill* SkillRegistry::find(const std::string& name) const {
    for (const auto& s : skills_) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}
