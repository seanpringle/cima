#pragma once

#include "config.h"

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Path sandbox
// ---------------------------------------------------------------------------
Result<std::string> resolve_path(const std::string& raw_path,
                                 const std::string& safe_dir);

// ---------------------------------------------------------------------------
// Tool
// ---------------------------------------------------------------------------
struct Tool {
    std::string name;
    std::string description;
    json parameters;
    std::function<Result<std::string>(const json& args)> execute;
};

// ---------------------------------------------------------------------------
// ToolRegistry
// ---------------------------------------------------------------------------
class ToolRegistry {
public:
    ToolRegistry() = default;

    void add(Tool tool);
    void add_defaults(const std::string& safe_dir);

    json to_openai_tools() const;
    Result<std::string> execute(const std::string& name,
                                const std::string& args_json);

    const std::vector<Tool>& tools() const { return tools_; }

private:
    Tool* find(const std::string& name);
    std::vector<Tool> tools_;
};
