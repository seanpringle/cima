#include "assistant_data.h"

#include <fstream>

using json = nlohmann::json;

// ===================================================================
// Serialization
// ===================================================================

json AssistantData::to_json() const {
    json j;
    j["version"] = version;
    j["name"] = name;
    j["model"] = model;
    j["conversation"] = conversation;
    j["chat_log"] = chat_log;
    j["plan"] = plan;
    j["notes"] = notes;
    return j;
}

void AssistantData::from_json(const json& j) {
    if (!j.is_object()) return;

    version = j.value("version", 1);
    name = j.value("name", std::string());
    model = j.value("model", std::string());
    conversation = j.value("conversation", json::array());
    chat_log = j.value("chat_log", json::array());
    plan = j.value("plan", json::object());
    notes = j.value("notes", json::object());
}

// ===================================================================
// File persistence
// ===================================================================

Result<void> AssistantData::save_to_file(const std::string& path) const {
    try {
        auto j = to_json();
        std::ofstream file(path);
        if (!file.is_open()) {
            return std::unexpected("Failed to open " + path + " for writing");
        }
        file << j.dump(2) << std::endl;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to save assistant data: ") + e.what());
    }
}

Result<void> AssistantData::load_from_file(const std::string& path) {
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
        return std::unexpected(std::string("Failed to load assistant data: ") + e.what());
    }
}
