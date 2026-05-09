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
Result<std::string> resolve_path(const std::string& raw_path, const std::string& safe_dir);

// ---------------------------------------------------------------------------
// Tool
// ---------------------------------------------------------------------------
struct Tool {
    std::string name;
    std::string description;
    json parameters;
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
    void add_defaults(const std::string& safe_dir);
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    json to_openai_tools() const;
    Result<std::string> execute(const std::string& name, const std::string& args_json);

    const std::vector<Tool>& tools() const { return tools_; }

  private:
    Tool* find(const std::string& name);
    std::vector<Tool> tools_;
    Mode mode_ = Mode::Build;
};
