#include "notes.h"

// ===================================================================
// Notes operations
// ===================================================================

Result<std::vector<int>> Notes::list_all_notes() {
    std::vector<int> ids;
    ids.reserve(notes_.size());
    for (const auto& [id, _] : notes_) {
        ids.push_back(id);
    }
    return ids;
}

Result<std::string> Notes::read_note(int id) {
    auto it = notes_.find(id);
    if (it == notes_.end()) {
        return std::unexpected("no such note: " + std::to_string(id));
    }
    return it->second;
}

Result<void> Notes::write_note(const std::string& body) {
    int id = static_cast<int>(notes_.size());
    notes_[id] = body;
    return {};
}

Result<void> Notes::delete_note(int id) {
    auto it = notes_.find(id);
    if (it == notes_.end()) {
        return std::unexpected("no such note: " + std::to_string(id));
    }
    notes_.erase(it);
    return {};
}

Result<void> Notes::delete_all_notes() {
    notes_.clear();
    return {};
}

// ===================================================================
// Serialization (for consolidated JSON)
// ===================================================================

json Notes::to_json() const {
    json j = json::object();
    for (const auto& [id, body] : notes_) {
        j[std::to_string(id)] = body;
    }
    return j;
}

void Notes::from_json(const json& j) {
    notes_.clear();
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string()) {
            try {
                int id = std::stoi(it.key());
                notes_[id] = it.value().get<std::string>();
            } catch (...) {
                // Non-integer key — skip (backward-compat with old string keys)
            }
        }
    }
}
