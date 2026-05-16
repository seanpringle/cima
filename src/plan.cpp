#include "plan.h"

#include <fstream>
#include <sstream>

// ===================================================================
// PlanBoard operations
// ===================================================================

Result<void> PlanBoard::write_plan(const std::string& markdown) {
    plan_ = markdown;
    // Auto-save is no longer performed here — external persistence handles it.
    return {};
}

Result<std::string> PlanBoard::read_plan() const {
    if (plan_.empty()) {
        return std::string("(empty plan)");
    }

    std::ostringstream ss;
    ss << "# Plan\n\n";
    ss << plan_ << "\n\n";

    return ss.str();
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
    if (!j.is_object()) return;
    plan_.clear();

    auto p = j.find("plan");
    if (p != j.end() && p->is_string()) {
        plan_ = p->get<std::string>();
    }
}

// ===================================================================
// File persistence (legacy)
// ===================================================================

Result<void> PlanBoard::save() {
    if (plan_file_path_.empty()) {
        return {};
    }
    auto j = to_json();
    std::ofstream file(plan_file_path_);
    if (!file.is_open()) {
        return std::unexpected("Cannot write plan file: " + plan_file_path_);
    }
    file << j.dump(2) << std::endl;
    return {};
}

Result<void> PlanBoard::load_from_file(const std::string& path) {
    plan_file_path_ = path;
    plan_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        // First run — no file yet, that's OK
        return {};
    }
    json j;
    try {
        file >> j;
    } catch (...) {
        // Corrupt file — start fresh
        return {};
    }
    if (j.is_object()) {
        auto p = j.find("plan");
        if (p != j.end() && p->is_string()) {
            plan_ = p->get<std::string>();
        }
    }
    return {};
}

// ===================================================================
// Tool: write_plan
// ===================================================================

Tool make_write_plan_tool(PlanBoard& board) {
    Tool t;
    t.name = "write_plan";
    t.description = "Write the Plan document. This completely replaces the plan "
                    "body. Use this from the Planner "
                    "to document the implementation plan for the Builder.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"markdown", {{"type", "string"}, {"description", "Markdown content of the plan"}}}}},
        {"required", {"markdown"}}};
    t.execute = [&board](const json& args) -> Result<std::string> {
        auto markdown = args.value("markdown", std::string());
        auto result = board.write_plan(markdown);
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
    t.description = "Read the Plan document. "
                    "Returns a markdown document with the plan.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {
        {"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&board](const json& /*args*/) -> Result<std::string> { return board.read_plan(); };
    return t;
}
