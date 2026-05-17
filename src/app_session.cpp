#include "app_session.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

// -----------------------------------------------------------------------
// Base directory helpers
// -----------------------------------------------------------------------

static std::filesystem::path get_home_dir() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) {
        home = std::getenv("USERPROFILE");
    }
    if (!home || !home[0]) {
        throw std::runtime_error("Cannot determine home directory (HOME not set)");
    }
    return std::filesystem::path(home);
}

std::filesystem::path AppSession::sessions_base_dir() {
    return get_home_dir() / ".local" / "state" / "cima";
}

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------

AppSession::AppSession(const std::string& name, bool force) : session_name_(name) {
    if (name.empty()) {
        throw std::invalid_argument("session name must not be empty");
    }
    // Sanity-check: no path separators in session name
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        throw std::invalid_argument("session name must not contain path separators");
    }

    session_dir_ = sessions_base_dir() / name;

    if (std::filesystem::exists(session_dir_)) {
        load_existing(force);
    } else {
        create_new();
    }
}

// -----------------------------------------------------------------------
// Load existing session
// -----------------------------------------------------------------------

void AppSession::load_existing(bool force) {
    (void)force; // kept for API compatibility

    if (!std::filesystem::is_directory(session_dir_)) {
        throw std::runtime_error("Session path exists but is not a directory: " +
                                 session_dir_.string());
    }

    auto manifest_path = session_dir_ / "state.json";
    if (!std::filesystem::exists(manifest_path)) {
        throw std::runtime_error("Session folder exists but is missing state.json: " +
                                 session_dir_.string());
    }

    // Read and parse state.json
    std::ifstream file(manifest_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open state.json: " + manifest_path.string());
    }
    json manifest;
    try {
        file >> manifest;
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to parse state.json: " + std::string(e.what()));
    }

    // New format: state.json is a JSON array of assistant filenames,
    // optionally with a top-level "last_cwd" string.
    if (manifest.is_array()) {
        assistant_files_.clear();
        for (const auto& item : manifest) {
            if (item.is_string()) {
                assistant_files_.push_back(item.get<std::string>());
            }
        }
        last_cwd_.clear();
    } else if (manifest.is_object()) {
        // Can be either:
        //   1. New format with "files" array + "last_cwd" (transitional)
        //   2. Old format with version/last_cwd/files
        // We support both by checking for "files" array and "last_cwd".
        last_cwd_ = manifest.value("last_cwd", std::string());

        if (manifest.contains("files") && manifest["files"].is_array()) {
            // New object format or old format — extract filenames
            assistant_files_.clear();
            for (const auto& item : manifest["files"]) {
                if (item.is_string()) {
                    assistant_files_.push_back(item.get<std::string>());
                }
            }
        } else {
            // No files array — start with empty list (don't scan directory,
            // which could resurrect orphan files as ghost tabs).
        }
    } else {
        throw std::runtime_error("Unexpected state.json format: expected array or object");
    }

    // ── Remove orphan .json files not listed in the manifest ──
    {
        std::error_code ec;
        std::filesystem::directory_iterator dir(session_dir_, ec);
        if (!ec) {
            for (const auto& entry : dir) {
                if (!entry.is_regular_file()) continue;
                auto fname = entry.path().filename().string();
                if (fname == "state.json") continue;
                if (fname.size() <= 5 ||
                    fname.substr(fname.size() - 5) != ".json")
                    continue;

                // If this file isn't in our assistant list, delete it
                if (std::find(assistant_files_.begin(), assistant_files_.end(),
                        fname) == assistant_files_.end()) {
                    std::filesystem::remove(entry.path(), ec); // ignore errors
                }
            }
        }
    }

    // Warn if last_cwd differs from current working directory
    if (!last_cwd_.empty()) {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        if (!ec && cwd.string() != last_cwd_) {
            std::cerr << "Warning: session " << session_name_
                      << " was last used in: " << last_cwd_ << "\n"
                      << "  Current directory: " << cwd.string() << "\n"
                      << "  (this is allowed — you can still proceed)" << std::endl;
        }
    }
}

// -----------------------------------------------------------------------
// Create new session
// -----------------------------------------------------------------------

void AppSession::create_new() {
    // Create the session directory
    std::error_code ec;
    if (!std::filesystem::create_directories(session_dir_, ec) && ec) {
        throw std::runtime_error("Failed to create session directory " +
                                 session_dir_.string() + ": " + ec.message());
    }

    // Get current working directory
    auto cwd = std::filesystem::current_path(ec);
    last_cwd_ = ec ? std::string() : cwd.string();

    // Write state.json
    save_manifest();

    is_new_ = true;
}

// -----------------------------------------------------------------------
// Manifest helpers
// -----------------------------------------------------------------------

void AppSession::add_assistant_file(const std::string& filename) {
    if (std::find(assistant_files_.begin(), assistant_files_.end(), filename) ==
        assistant_files_.end()) {
        assistant_files_.push_back(filename);
        save_manifest();
    }
}

void AppSession::remove_assistant_file(const std::string& filename) {
    auto it = std::remove(assistant_files_.begin(), assistant_files_.end(), filename);
    if (it != assistant_files_.end()) {
        assistant_files_.erase(it, assistant_files_.end());
        save_manifest();
    }
}

void AppSession::set_assistant_files(const std::vector<std::string>& files) {
    assistant_files_ = files;
    save_manifest();
}

// -----------------------------------------------------------------------
// Path accessors
// -----------------------------------------------------------------------

std::string AppSession::wiki_dir_path() const {
    return (session_dir_ / "wiki").string();
}

std::string AppSession::session_file_path(const std::string& filename) const {
    return (session_dir_ / filename).string();
}

// -----------------------------------------------------------------------
// Manifest data
// -----------------------------------------------------------------------

void AppSession::set_last_cwd(const std::string& cwd) {
    last_cwd_ = cwd;
}

// -----------------------------------------------------------------------
// Save manifest
// -----------------------------------------------------------------------

void AppSession::save_manifest() {
    json manifest = json::array();
    for (const auto& fname : assistant_files_) {
        manifest.push_back(fname);
    }

    // Only add last_cwd if non-empty
    if (!last_cwd_.empty()) {
        // Use an object wrapper to include last_cwd alongside the list.
        // We store the assistant list under a "files" key.
        json obj;
        obj["files"] = std::move(manifest);
        obj["last_cwd"] = last_cwd_;
        manifest = std::move(obj);
    }

    auto manifest_path = session_dir_ / "state.json";
    std::ofstream file(manifest_path);
    if (!file.is_open()) {
        std::cerr << "Error: cannot write state.json: " << manifest_path.string() << std::endl;
        return;
    }
    file << manifest.dump(2) << std::endl;
}

// -----------------------------------------------------------------------
// Welcome message
// -----------------------------------------------------------------------

void AppSession::print_welcome() const {
    if (is_new_) {
        std::cout << "Starting new session '" << session_name_ << "'\n"
                  << "  Session directory: " << session_dir_.string() << "\n";
    } else {
        std::cout << "Resuming session '" << session_name_ << "'\n"
                  << "  Session directory: " << session_dir_.string() << "\n";
    }
    std::cout << std::flush;
}
