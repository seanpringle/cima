#include "notes.h"

#include <algorithm>
#include <fstream>
#include <sstream>

// ===================================================================
// Notes operations
// ===================================================================

Result<std::vector<std::string>> Notes::list_all_notes() {
    std::vector<std::string> names;
    names.reserve(notes_.size());
    for (const auto& [name, _] : notes_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

Result<std::string> Notes::read_note(const std::string& name) {
    auto it = notes_.find(name);
    if (it == notes_.end()) {
        return std::unexpected("no such note: " + name);
    }
    return it->second;
}

Result<void> Notes::write_note(const std::string& name, const std::string& body) {
    notes_[name] = body;
    if (!notes_file_path_.empty()) {
        auto r = save();
        if (!r) return r;
    }
    return {};
}

Result<void> Notes::delete_note(const std::string& name) {
    auto it = notes_.find(name);
    if (it == notes_.end()) {
        return std::unexpected("no such note: " + name);
    }
    notes_.erase(it);
    if (!notes_file_path_.empty()) {
        auto r = save();
        if (!r) return r;
    }
    return {};
}

Result<void> Notes::delete_all_notes() {
    notes_.clear();
    if (!notes_file_path_.empty()) {
        auto r = save();
        if (!r) return r;
    }
    return {};
}

// ===================================================================
// File persistence
// ===================================================================

Result<void> Notes::save() {
    if (notes_file_path_.empty()) {
        return {};
    }
    json j = json::object();
    for (const auto& [name, body] : notes_) {
        j[name] = body;
    }
    std::ofstream file(notes_file_path_);
    if (!file.is_open()) {
        return std::unexpected("Cannot write notes file: " + notes_file_path_);
    }
    file << j.dump(2) << std::endl;
    return {};
}

Result<void> Notes::load_from_file(const std::string& path) {
    notes_file_path_ = path;
    notes_.clear();

    std::ifstream file(path);
    if (!file.is_open()) {
        // First run — no file yet, that's OK
        return {};
    }
    json j;
    try {
        file >> j;
    } catch (...) {
        // Corrupt file — start fresh
        return {};
    }
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                notes_[it.key()] = it.value().get<std::string>();
            }
        }
    }
    return {};
}
