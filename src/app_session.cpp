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
// Home directory helper
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
    (void)force;

    if (name.empty()) {
        throw std::invalid_argument("session name must not be empty");
    }
    // Sanity-check: no path separators in session name
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        throw std::invalid_argument("session name must not contain path separators");
    }

    // Ensure the base directory exists
    std::error_code ec;
    std::filesystem::create_directories(sessions_base_dir(), ec);
    if (ec) {
        throw std::runtime_error("Failed to create sessions directory " +
                                 sessions_base_dir().string() + ": " + ec.message());
    }

    // Check if the session file already exists
    auto session_path = std::filesystem::path(session_file_path());
    if (std::filesystem::exists(session_path)) {
        // Existing session — load last_cwd from it
        auto result = load_session();
        if (result) {
            last_cwd_ = result->last_cwd;
        }
        // If load fails, we still proceed (last_cwd will be empty)
    } else {
        // Check for old directory-format session to migrate
        auto old_dir = sessions_base_dir() / name;
        if (std::filesystem::is_directory(old_dir) &&
            std::filesystem::exists(old_dir / "state.json")) {
            if (!try_migrate_old_format()) {
                // Migration failed — will start fresh
                std::cerr << "Warning: failed to migrate old-format session '"
                          << name << "'; starting new session." << std::endl;
            }
        }

        // If the file still doesn't exist (fresh session or failed migration),
        // record last_cwd for the welcome message
        if (!std::filesystem::exists(session_path)) {
            auto cwd = std::filesystem::current_path(ec);
            last_cwd_ = ec ? std::string() : cwd.string();
            is_new_ = true;
        } else {
            // Migration succeeded and created the file — load last_cwd
            auto result = load_session();
            if (result) {
                last_cwd_ = result->last_cwd;
            }
        }
    }
}

// -----------------------------------------------------------------------
// Path accessors
// -----------------------------------------------------------------------

std::string AppSession::session_file_path() const {
    return (sessions_base_dir() / (session_name_ + ".json")).string();
}

std::string AppSession::backup_dir_path() const {
    return (sessions_base_dir() / (session_name_ + ".bak")).string();
}

// -----------------------------------------------------------------------
// Session data persistence
// -----------------------------------------------------------------------

Result<void> AppSession::save_session(const SessionData& data) {
    try {
        auto path = std::filesystem::path(session_file_path());
        auto tmp_path = path;
        tmp_path += ".tmp";

        // Write to temporary file
        {
            std::ofstream file(tmp_path);
            if (!file.is_open()) {
                return std::unexpected("Failed to open temporary file for writing: " +
                                       tmp_path.string());
            }
            file << data.to_json().dump(2) << std::endl;
        }

        // Atomic rename (POSIX)
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            // Clean up temp file on failure
            std::filesystem::remove(tmp_path, ec);
            return std::unexpected("Failed to rename session file: " + ec.message());
        }

        last_cwd_ = data.last_cwd;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to save session: ") + e.what());
    }
}

Result<SessionData> AppSession::load_session() {
    auto path = session_file_path();
    SessionData data;
    auto result = data.load_from_file(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    return data;
}

// -----------------------------------------------------------------------
// Migration from old directory format
// -----------------------------------------------------------------------

bool AppSession::try_migrate_old_format() {
    std::error_code ec;
    auto old_dir = sessions_base_dir() / session_name_;

    if (!std::filesystem::is_directory(old_dir, ec) || ec) {
        return false;
    }

    auto manifest_path = old_dir / "state.json";
    if (!std::filesystem::exists(manifest_path, ec)) {
        return false;
    }

    // Read old state.json to get last_cwd and assistant file list
    std::ifstream manifest_file(manifest_path);
    if (!manifest_file.is_open()) {
        return false;
    }

    json manifest;
    try {
        manifest_file >> manifest;
    } catch (...) {
        return false;
    }

    std::vector<std::string> assistant_filenames;
    std::string old_last_cwd;

    if (manifest.is_array()) {
        for (const auto& item : manifest) {
            if (item.is_string()) {
                assistant_filenames.push_back(item.get<std::string>());
            }
        }
    } else if (manifest.is_object()) {
        old_last_cwd = manifest.value("last_cwd", std::string());
        if (manifest.contains("files") && manifest["files"].is_array()) {
            for (const auto& item : manifest["files"]) {
                if (item.is_string()) {
                    assistant_filenames.push_back(item.get<std::string>());
                }
            }
        }
    } else {
        return false;
    }

    // Find the most recently modified assistant .json file
    std::string newest_file;
    std::filesystem::file_time_type newest_time;

    for (const auto& fname : assistant_filenames) {
        auto fpath = old_dir / fname;
        if (!std::filesystem::exists(fpath, ec)) continue;
        auto ftime = std::filesystem::last_write_time(fpath, ec);
        if (ec) continue;
        if (newest_file.empty() || ftime > newest_time) {
            newest_file = fname;
            newest_time = ftime;
        }
    }

    // Build new SessionData from the newest assistant file
    SessionData data;
    data.last_cwd = old_last_cwd;

    if (!newest_file.empty()) {
        auto fpath = old_dir / newest_file;
        std::ifstream afile(fpath);
        if (afile.is_open()) {
            try {
                json aj;
                afile >> aj;
                // The old AssistantData format had these fields at top level
                data.provider_name = aj.value("provider_name", std::string());
                data.model = aj.value("model", std::string());
                data.reasoning_effort = aj.value("reasoning_effort", std::string());
                data.workspace_path = aj.value("workspace_path", std::string());
                data.bash_enabled = aj.value("bash_enabled", false);

                // Conversation, chat_log, plan are already json fields
                if (aj.contains("conversation")) data.conversation = aj["conversation"];
                if (aj.contains("chat_log")) data.chat_log = aj["chat_log"];
                if (aj.contains("plan")) data.plan = aj["plan"];

                // MCP enabled state was never persisted in old format — start empty
            } catch (...) {
                // Corrupt assistant file — proceed with defaults
            }
        }
    }

    // Write the new format file
    auto new_path = std::filesystem::path(session_file_path());
    {
        std::ofstream file(new_path);
        if (!file.is_open()) {
            return false;
        }
        file << data.to_json().dump(2) << std::endl;
    }

    // Rename old directory to .bak/ suffix
    auto backup_path = sessions_base_dir() / (session_name_ + ".bak");
    std::filesystem::rename(old_dir, backup_path, ec);
    if (ec) {
        // Migration partial — warn but consider it done (new file exists)
        std::cerr << "Warning: migrated session data but could not rename old directory: "
                  << ec.message() << std::endl;
    }

    return true;
}

// -----------------------------------------------------------------------
// Metadata
// -----------------------------------------------------------------------

void AppSession::print_welcome() const {
    auto session_path = session_file_path();
    if (is_new_) {
        std::cout << "Starting new session '" << session_name_ << "'\n"
                  << "  Session file: " << session_path << "\n";
    } else {
        std::cout << "Resuming session '" << session_name_ << "'\n"
                  << "  Session file: " << session_path << "\n";
    }
    std::cout << std::flush;
}
