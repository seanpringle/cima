#include "session_data.h"

#include <fstream>

using json = nlohmann::json;

// ===================================================================
// Serialization
// ===================================================================

json SessionData::to_json() const {
    json j;
    j["version"] = version;
    j["last_cwd"] = last_cwd;
    j["provider_name"] = provider_name;
    j["model"] = model;
    j["reasoning_effort"] = reasoning_effort;
    // workspace_path removed — safe_dir locked to cwd
    j["conversation"] = conversation;
    j["chat_log"] = chat_log;
    j["plan"] = plan;
    j["bash_enabled"] = bash_enabled;
    j["cmake_enabled"] = cmake_enabled;

    // Serialise mcp_enabled map
    json mcp = json::object();
    for (const auto& [name, enabled] : mcp_enabled) {
        mcp[name] = enabled;
    }
    j["mcp_enabled"] = std::move(mcp);

    // Serialise cmd_tools_enabled map
    json cte = json::object();
    for (const auto& [name, enabled] : cmd_tools_enabled) {
        cte[name] = enabled;
    }
    j["cmd_tools_enabled"] = std::move(cte);

    // Serialise snippets map
    json snip = json::object();
    for (const auto& [name, content] : snippets) {
        snip[name] = content;
    }
    j["snippets"] = std::move(snip);

    // Serialise custom_commands map
    json cc = json::object();
    for (const auto& [name, cmd] : custom_commands) {
        json entry;
        entry["description"] = cmd.description;
        entry["command"] = cmd.command;
        cc[name] = std::move(entry);
    }
    j["custom_commands"] = std::move(cc);

    return j;
}

void SessionData::from_json(const json& j) {
    if (!j.is_object())
        return;

    version = j.value("version", 2);
    last_cwd = j.value("last_cwd", std::string());
    provider_name = j.value("provider_name", std::string());
    model = j.value("model", std::string());
    reasoning_effort = j.value("reasoning_effort", std::string());
    // workspace_path removed — safe_dir locked to cwd
    conversation = j.value("conversation", json::array());
    chat_log = j.value("chat_log", json::array());
    plan = j.value("plan", json::object());
    bash_enabled = j.value("bash_enabled", false);
    cmake_enabled = j.value("cmake_enabled", false);

    // Deserialise mcp_enabled map
    mcp_enabled.clear();
    if (j.contains("mcp_enabled") && j["mcp_enabled"].is_object()) {
        for (auto it = j["mcp_enabled"].begin(); it != j["mcp_enabled"].end(); ++it) {
            if (it.value().is_boolean()) {
                mcp_enabled[it.key()] = it.value().get<bool>();
            }
        }
    }

    // Deserialise cmd_tools_enabled map
    cmd_tools_enabled.clear();
    if (j.contains("cmd_tools_enabled") && j["cmd_tools_enabled"].is_object()) {
        for (auto it = j["cmd_tools_enabled"].begin(); it != j["cmd_tools_enabled"].end(); ++it) {
            if (it.value().is_boolean()) {
                cmd_tools_enabled[it.key()] = it.value().get<bool>();
            }
        }
    }

    // Deserialise snippets map
    snippets.clear();
    if (j.contains("snippets") && j["snippets"].is_object()) {
        for (auto it = j["snippets"].begin(); it != j["snippets"].end(); ++it) {
            if (it.value().is_string()) {
                snippets[it.key()] = it.value().get<std::string>();
            }
        }
    }

    // Deserialise custom_commands map
    custom_commands.clear();
    if (j.contains("custom_commands") && j["custom_commands"].is_object()) {
        for (auto it = j["custom_commands"].begin(); it != j["custom_commands"].end(); ++it) {
            if (it.value().is_object()) {
                CmdToolConfig cmd;
                cmd.name = it.key();
                cmd.description = it.value().value("description", std::string());
                cmd.command = it.value().value("command", std::string());
                if (!cmd.name.empty() && !cmd.command.empty()) {
                    custom_commands[it.key()] = std::move(cmd);
                }
            }
        }
    }
}

// ===================================================================
// File persistence
// ===================================================================

Result<void> SessionData::save_to_file(const std::string& path) const {
    try {
        auto j = to_json();
        std::ofstream file(path);
        if (!file.is_open()) {
            return std::unexpected("Failed to open " + path + " for writing");
        }
        file << j.dump(2) << std::endl;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to save session data: ") + e.what());
    }
}

Result<void> SessionData::load_from_file(const std::string& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected("Failed to open " + path + " for reading");
        }
        json j;
        file >> j;
        from_json(j);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to load session data: ") + e.what());
    }
}
