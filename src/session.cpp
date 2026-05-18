#include "session.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unistd.h>

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

std::filesystem::path Session::sessions_base_dir() {
    return get_home_dir() / ".local" / "state" / "cima";
}

// -----------------------------------------------------------------------
// Constructor
// -----------------------------------------------------------------------

Session::Session(const std::string& name) : session_name_{name} {
    if (name.empty()) {
        throw std::invalid_argument("session name must not be empty");
    }
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
            last_cwd_ = session_data_.last_cwd;
        }
    } else {
        // Fresh session — record current working directory
        auto cwd = std::filesystem::current_path(ec);
        last_cwd_ = ec ? std::string() : cwd.string();
        is_new_ = true;
    }
}

Session::~Session() { save_session(); }

// -----------------------------------------------------------------------
// Path accessors
// -----------------------------------------------------------------------

std::string Session::session_file_path() const {
    return (sessions_base_dir() / (session_name_ + ".json")).string();
}

// -----------------------------------------------------------------------
// Session data persistence
// -----------------------------------------------------------------------

Result<void> Session::save_session() {
    try {
        auto path = std::filesystem::path(session_file_path());
        auto tmp_path = path;
        tmp_path += ".tmp";

        // Write to temporary file
        {
            std::ofstream file(tmp_path);
            if (!file.is_open()) {
                return std::unexpected(
                    "Failed to open temporary file for writing: " + tmp_path.string());
            }
            file << session_data_.to_json().dump(2) << std::endl;
        }

        // Atomic rename (POSIX)
        std::error_code ec;
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            std::filesystem::remove(tmp_path, ec);
            return std::unexpected("Failed to rename session file: " + ec.message());
        }

        last_cwd_ = session_data_.last_cwd;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to save session: ") + e.what());
    }
}

Result<void> Session::load_session() {
    auto path = session_file_path();
    auto result = session_data_.load_from_file(path);
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

// -----------------------------------------------------------------------
// Metadata
// -----------------------------------------------------------------------

void Session::print_welcome() const {
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
