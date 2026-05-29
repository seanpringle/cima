#include "plan.h"

// ===================================================================
// PlanBoard operations
// ===================================================================

Result<void> PlanBoard::write_plan(const std::string& markdown) {
    plan_ = markdown;
    return {};
}

Result<std::string> PlanBoard::read_plan() const {
    if (plan_.empty()) {
        return std::string("(empty plan)");
    }

    return plan_;
}

// ===================================================================
// Serialization (for consolidated JSON)
// ===================================================================

json PlanBoard::to_json() const {
    json j;
    j["plan"] = plan_;
    return j;
}

void PlanBoard::from_json(const json& j) {
    // Silence: tolerate non-object input (empty plan) for backwards
    // compatibility with earlier formats that may not have a plan key.
    if (!j.is_object())
        return;
    plan_.clear();

    auto p = j.find("plan");
    if (p != j.end() && p->is_string()) {
        plan_ = p->get<std::string>();
    }
}

// ===================================================================
// Tool: write_plan
// ===================================================================

Tool make_write_plan_tool(PlanBoard& board) {
    Tool t;
    t.name = "write_plan";
    t.description = "Write the Plan markdown document. This completely replaces the plan body.";

    t.permission = ToolPermission::Write;

    t.parameters = {{"type", "object"},
        {"properties", {{"content", {{"type", "string"}, {"description", "Markdown content of the plan"}}}}},
        {"required", {"content"}}};

    t.execute = [&board](const json& args) -> Result<std::string> {
        for (auto& el : args.items()) {
            if (el.key() == "content")
                continue;
            return std::unexpected("unknown argument: " + el.key());
        }
        if (!args.contains("content")) {
            return std::unexpected("missing argument: content");
        }

        auto content = args.value("content", std::string());
        auto result = board.write_plan(content);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Plan written. Use read_plan() to view it.";
    };
    return t;
}

// ===================================================================
// Tool: read_plan
// ===================================================================

Tool make_read_plan_tool(PlanBoard& board) {
    Tool t;
    t.name = "read_plan";
    t.description = "Read the Plan markdown document.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&board](const json& /*args*/) -> Result<std::string> { return board.read_plan(); };
    return t;
}
