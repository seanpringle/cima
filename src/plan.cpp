#include "plan.h"

#include <fstream>
#include <sstream>

// ===================================================================
// PlanBoard operations
// ===================================================================

Result<void> PlanBoard::write_plan(const std::string& markdown) {
    plan_ = markdown;
    // Clear comments when a new plan is written
    comments_.clear();
    // Auto-save
    if (!plan_file_path_.empty()) {
        auto r = save();
        if (!r) return r;
    }
    return {};
}

Result<std::string> PlanBoard::read_plan() const {
    if (plan_.empty()) {
        return std::string("(empty plan)");
    }

    std::ostringstream ss;
    ss << "# Plan\n\n";
    ss << plan_ << "\n\n";

    if (!comments_.empty()) {
        ss << "---\n\n## Comments\n\n";
        for (size_t i = 0; i < comments_.size(); i++) {
            ss << "### Comment " << (i + 1) << "\n\n";
            ss << comments_[i] << "\n\n";
        }
    }

    return ss.str();
}

Result<void> PlanBoard::comment_plan(const std::string& markdown) {
    if (markdown.empty()) {
        return std::unexpected(std::string("comment must not be empty"));
    }

    comments_.push_back(markdown);
    // Auto-save
    if (!plan_file_path_.empty()) {
        auto r = save();
        if (!r) return r;
    }
    return {};
}

// ===================================================================
// File persistence
// ===================================================================

Result<void> PlanBoard::save() {
    if (plan_file_path_.empty()) {
        return {};
    }
    json j;
    j["plan"] = plan_;
    j["comments"] = json::array();
    for (const auto& c : comments_) {
        j["comments"].push_back(c);
    }
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
    comments_.clear();

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
        auto c = j.find("comments");
        if (c != j.end() && c->is_array()) {
            for (const auto& item : *c) {
                if (item.is_string()) {
                    comments_.push_back(item.get<std::string>());
                }
            }
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
                    "body (comments are preserved separately). Use this from the Planner "
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
    t.description = "Read the Plan document (plan body + comments). "
                    "Returns a markdown document with the plan and any comments.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {
        {"type", "object"}, {"properties", json::object()}, {"required", json::array()}};
    t.execute = [&board](const json& /*args*/) -> Result<std::string> { return board.read_plan(); };
    return t;
}

// ===================================================================
// Tool: comment_plan
// ===================================================================

Tool make_comment_plan_tool(PlanBoard& board) {
    Tool t;
    t.name = "comment_plan";
    t.description = "Append a comment to the Plan document. Comments are preserved "
                    "separately from the plan body and listed after it. Use this for "
                    "progress updates, review feedback, or change requests.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"comment",
                {{"type", "string"}, {"description", "Markdown comment to append to the plan"}}}}},
        {"required", {"comment"}}};
    t.execute = [&board](const json& args) -> Result<std::string> {
        auto comment = args.value("comment", std::string());
        auto result = board.comment_plan(comment);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "Comment added to plan.";
    };
    return t;
}
