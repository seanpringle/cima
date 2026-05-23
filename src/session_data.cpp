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

    // Serialise tool_gates map
    json tg = json::object();
    for (const auto& [name, enabled] : tool_gates) {
        tg[name] = enabled;
    }
    j["tool_gates"] = std::move(tg);

    // Serialise rw_subagent_tool_gates map
    json rwtg = json::object();
    for (const auto& [name, enabled] : rw_subagent_tool_gates) {
        rwtg[name] = enabled;
    }
    j["rw_subagent_tool_gates"] = std::move(rwtg);

    // Serialise ro_subagent_tool_gates map
    json rotg = json::object();
    for (const auto& [name, enabled] : ro_subagent_tool_gates) {
        rotg[name] = enabled;
    }
    j["ro_subagent_tool_gates"] = std::move(rotg);

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

    // Serialise custom_mcp_servers vector
    json mcp_arr = json::array();
    for (const auto& m : custom_mcp_servers) {
        json mj;
        mj["name"] = m.name;
        mj["transport"] = m.transport;
        mj["command"] = m.command;
        mj["args"] = m.args;
        mj["cwd"] = m.cwd;
        mj["url"] = m.url;
        mj["api_key"] = m.api_key;
        mj["env"] = m.env;
        mj["timeout_sec"] = m.timeout_sec;
        mcp_arr.push_back(std::move(mj));
    }
    j["custom_mcp_servers"] = std::move(mcp_arr);

    // Serialise input_history
    json hist = json::array();
    for (const auto& item : input_history) {
        hist.push_back(item);
    }
    j["input_history"] = std::move(hist);

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

    // Deserialise tool_gates map
    tool_gates.clear();
    if (j.contains("tool_gates") && j["tool_gates"].is_object()) {
        for (auto it = j["tool_gates"].begin(); it != j["tool_gates"].end(); ++it) {
            if (it.value().is_boolean()) {
                tool_gates[it.key()] = it.value().get<bool>();
            }
        }
    }

    // Deserialise rw_subagent_tool_gates map
    rw_subagent_tool_gates.clear();
    if (j.contains("rw_subagent_tool_gates") && j["rw_subagent_tool_gates"].is_object()) {
        for (auto it = j["rw_subagent_tool_gates"].begin(); it != j["rw_subagent_tool_gates"].end();
            ++it) {
            if (it.value().is_boolean()) {
                rw_subagent_tool_gates[it.key()] = it.value().get<bool>();
            }
        }
    }

    // Deserialise ro_subagent_tool_gates map
    ro_subagent_tool_gates.clear();
    if (j.contains("ro_subagent_tool_gates") && j["ro_subagent_tool_gates"].is_object()) {
        for (auto it = j["ro_subagent_tool_gates"].begin(); it != j["ro_subagent_tool_gates"].end();
            ++it) {
            if (it.value().is_boolean()) {
                ro_subagent_tool_gates[it.key()] = it.value().get<bool>();
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

    // Deserialise custom_mcp_servers array
    custom_mcp_servers.clear();
    if (j.contains("custom_mcp_servers") && j["custom_mcp_servers"].is_array()) {
        for (const auto& mj : j["custom_mcp_servers"]) {
            McpEndpoint m;
            m.name = mj.value("name", std::string());
            m.transport = mj.value("transport", std::string("stdio"));
            m.command = mj.value("command", std::string());
            m.url = mj.value("url", std::string());
            m.api_key = mj.value("api_key", std::string());
            m.cwd = mj.value("cwd", std::string());
            m.timeout_sec = mj.value("timeout_sec", 60);

            if (mj.contains("args") && mj["args"].is_array()) {
                for (const auto& a : mj["args"]) {
                    if (a.is_string())
                        m.args.push_back(a.get<std::string>());
                }
            }

            if (mj.contains("env") && mj["env"].is_object()) {
                for (auto it = mj["env"].begin(); it != mj["env"].end(); ++it) {
                    if (it.value().is_string())
                        m.env[it.key()] = it.value().get<std::string>();
                }
            }

            if (!m.name.empty()) {
                custom_mcp_servers.push_back(std::move(m));
            }
        }
    }

    // Deserialise input_history
    input_history.clear();
    if (j.contains("input_history") && j["input_history"].is_array()) {
        for (const auto& item : j["input_history"]) {
            if (item.is_string()) {
                input_history.push_back(item.get<std::string>());
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
