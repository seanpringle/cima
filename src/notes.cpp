#include "notes.h"

// ===================================================================
// Notes operations
// ===================================================================

Result<std::vector<int>> Notes::list_notes() {
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

Result<int> Notes::write_note(const std::string& body, std::optional<int> id) {
    int assigned = id.value_or(next_id_);
    notes_[assigned] = body;
    if (!id) {
        // Only advance next_id_ for auto-assigned IDs, not explicit overrides.
        next_id_ = assigned + 1;
    } else {
        // Ensure next_id_ stays past any explicit ID.
        if (assigned >= next_id_) {
            next_id_ = assigned + 1;
        }
    }
    return assigned;
}

Result<void> Notes::delete_note(int id) {
    auto it = notes_.find(id);
    if (it == notes_.end()) {
        return std::unexpected("no such note: " + std::to_string(id));
    }
    notes_.erase(it);
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
    j["_next_id"] = next_id_;
    return j;
}

void Notes::from_json(const json& j) {
    notes_.clear();
    next_id_ = 1;
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

    // Restore next_id_ from persisted field, or compute from max existing ID.
    auto nid_it = j.find("_next_id");
    if (nid_it != j.end() && nid_it->is_number_integer()) {
        next_id_ = nid_it->get<int>();
    } else if (!notes_.empty()) {
        next_id_ = notes_.rbegin()->first + 1;
    }
}
