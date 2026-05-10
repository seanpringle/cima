#pragma once

#include "config.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Path sandbox
// ---------------------------------------------------------------------------
Result<std::string> resolve_path(const std::string& raw_path, const std::string& safe_dir);

// ---------------------------------------------------------------------------
// ToolPermission — which permission category a tool belongs to
// ---------------------------------------------------------------------------
enum class ToolPermission { ReadOnly, Write, Internal };

// ---------------------------------------------------------------------------
// Tool
// ---------------------------------------------------------------------------
struct Tool {
    std::string name;
    std::string description;
    json parameters;
    ToolPermission permission = ToolPermission::Write;
    int timeout_sec = 0; // 0 = no timeout
    std::function<Result<std::string>(const json& args)> execute;
};

// ---------------------------------------------------------------------------
// Mode — controls which tools are allowed at runtime
// ---------------------------------------------------------------------------
enum class Mode { Plan, Build };

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------
class ToolRegistry {
  public:
    ToolRegistry() = default;

    void add(Tool tool);
    void add_defaults(const std::string& safe_dir,
        const std::string& search_api_key = {},
        const std::string& search_engine_id = {},
        const std::string& search_endpoint = {});
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    json to_openai_tools() const;
    /// Return tools for OpenAI, filtered to only include tools whose names
    /// appear in \p only_these (if non-null).
    json to_openai_tools(const std::set<std::string>* only_these) const;
    Result<std::string> execute(const std::string& name, const std::string& args_json);

    const std::vector<Tool>& tools() const { return tools_; }

    /// Return the names of all registered tools with the given permission.
    std::set<std::string> tool_names_by_permission(ToolPermission perm) const;

  private:
    Tool* find(const std::string& name);
    std::vector<Tool> tools_;
    Mode mode_ = Mode::Build;
};
