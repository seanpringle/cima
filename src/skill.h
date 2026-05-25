#pragma once

#include "types.h"

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Skill — represents a single skill discovered from ~/.agents/skills/<name>/SKILL.md
// ---------------------------------------------------------------------------
struct Skill {
    std::string name;        // from YAML frontmatter `name`
    std::string description; // from YAML frontmatter `description`
    std::string body;        // markdown content after frontmatter
    std::string dir;         // full path to skill directory
};

// ---------------------------------------------------------------------------
// SkillRegistry — scans ~/.agents/skills/ and provides lookup
// ---------------------------------------------------------------------------
class SkillRegistry {
  public:
    /// Scan ~/.agents/skills/ for skill directories containing SKILL.md.
    /// If the directory does not exist this is a no-op (empty registry).
    void scan();

    /// Look up a skill by name. Returns nullptr if not found.
    const Skill* find(const std::string& name) const;

    /// Return all discovered skills.
    const std::vector<Skill>& skills() const { return skills_; }

    /// Return the number of discovered skills.
    size_t size() const { return skills_.size(); }

  private:
    std::vector<Skill> skills_;
};
