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
Result<std::string> resolve_path(const std::string& raw_path,
    const std::string& safe_dir,
    const std::vector<std::string>& extra_allowed = {});

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
// ToolRegistry
// ---------------------------------------------------------------------------
class ToolRegistry {
  public:
    ToolRegistry() = default;

    void set_cancelled(CancellationToken t) { cancelled_ = std::move(t); }

    void add(Tool tool);
    void add_defaults(const std::string& safe_dir,
        const std::vector<std::string>& read_only_paths = {},
        const std::string& search_api_key = {},
        const std::string& search_engine_id = {},
        const std::string& search_endpoint = {},
        bool include_write = true);

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
    CancellationToken cancelled_;
    std::vector<Tool> tools_;
};
