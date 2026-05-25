#include "chat.h"
#include "skill.h"
#include "tools.h"

// ===================================================================
// Tool: load_skill
// ===================================================================

Tool make_load_skill_tool(SkillRegistry& registry, ChatSession& session) {
    Tool t;
    t.name = "load_skill";
    t.description =
        "Load a skill's instruction body into context. The skill body is injected "
        "as a system instruction that guides subsequent responses. Available skills "
        "are listed in the system prompt above.";
    t.permission = ToolPermission::ReadOnly;

    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"},
                    {"description", "Name of the skill to load (see the Available Skills "
                                    "table in the system prompt for names)"}}}}},
        {"required", {"name"}}};

    t.execute = [&registry, &session](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());

        if (name.empty())
            return std::unexpected("load_skill: skill name must not be empty");

        const Skill* skill = registry.find(name);
        if (!skill)
            return std::unexpected("load_skill: unknown skill \"" + name + "\"");

        // Inject the skill body as a system message (droppable, so compaction
        // can remove it when context is tight).
        session.conversation().add_system(
            "## Skill: " + skill->name + "\n\n" + skill->body, "droppable");

        return "skill \"" + name + "\" loaded";
    };

    return t;
}
