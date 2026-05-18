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

    // Serialise mcp_enabled map
    json mcp = json::object();
    for (const auto& [name, enabled] : mcp_enabled) {
        mcp[name] = enabled;
    }
    j["mcp_enabled"] = std::move(mcp);

    return j;
}

void SessionData::from_json(const json& j) {
    if (!j.is_object()) return;

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

    // Deserialise mcp_enabled map
    mcp_enabled.clear();
    if (j.contains("mcp_enabled") && j["mcp_enabled"].is_object()) {
        for (auto it = j["mcp_enabled"].begin(); it != j["mcp_enabled"].end(); ++it) {
            if (it.value().is_boolean()) {
                mcp_enabled[it.key()] = it.value().get<bool>();
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
