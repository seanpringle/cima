#include "app_session.h"
#include "ship_name.h"

#include <sqlite3.h>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

    // Validate version
    int version = manifest.value("version", 0);
    if (version != 1) {
        throw std::runtime_error("Unsupported state.json version: " + std::to_string(version));
    }

    // Read last_cwd
    last_cwd_ = manifest.value("last_cwd", std::string());

    // Read file list
    std::vector<std::string> listed_files;
    if (manifest.contains("files") && manifest["files"].is_array()) {
        for (const auto& f : manifest["files"]) {
            if (f.is_string()) {
                listed_files.push_back(f.get<std::string>());
            }
        }
    }

    // Integrity check — may rewrite manifest on disk if force=true
    bool had_issues = verify_integrity(force);

    // If force corrected the manifest, re-read the file list
    if (had_issues && force) {
        listed_files.clear();
        std::ifstream updated(manifest_path);
        json updated_manifest;
        if (updated.is_open()) {
            try {
                updated >> updated_manifest;
            } catch (...) {
            }
        }
        if (updated_manifest.contains("files") && updated_manifest["files"].is_array()) {
            for (const auto& f : updated_manifest["files"]) {
                if (f.is_string()) {
                    listed_files.push_back(f.get<std::string>());
                }
            }
        }
    }

    // Populate agent DBs (all .db files except wiki.db)
    agent_dbs_.clear();
    for (const auto& fname : listed_files) {
        if (fname != "wiki.db" && fname.size() >= 3 &&
            fname.substr(fname.size() - 3) == ".db") {
            agent_dbs_.push_back(fname);
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

    // Generate a random agent name for the first tab
    std::string agent_name = generate_lotr_name();
    std::string agent_filename = agent_name + ".db";
    agent_dbs_.push_back(agent_filename);

    // Create wiki.db (empty database with tables)
    {
        std::string wiki_path = wiki_db_path();
        // Opening the database creates the file; the Wiki constructor
        // will initialize the tables when opened.
        sqlite3* wiki_db = nullptr;
        int rc = sqlite3_open(wiki_path.c_str(), &wiki_db);
        if (rc != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(wiki_db);
            sqlite3_close(wiki_db);
            throw std::runtime_error("Failed to create wiki.db: " + msg);
        }
        // Initialize tables (same SQL as Wiki::init_tables)
        const char* sql = R"(
            CREATE TABLE IF NOT EXISTS wiki_pages (
                title      TEXT PRIMARY KEY,
                body       TEXT NOT NULL DEFAULT '',
                updated_at TEXT NOT NULL DEFAULT (datetime('now'))
            )
        )";
        char* err = nullptr;
        rc = sqlite3_exec(wiki_db, sql, nullptr, nullptr, &err);
        if (rc != SQLITE_OK) {
            std::string msg = err ? err : "unknown";
            sqlite3_free(err);
            sqlite3_close(wiki_db);
            throw std::runtime_error("Failed to initialize wiki.db: " + msg);
        }
        sqlite3_close(wiki_db);
    }

    // Create the first agent DB (empty, will be populated when ChatSession opens it)
    {
        std::string agent_path = agent_db_path(agent_filename);
        sqlite3* agent_db = nullptr;
        int rc = sqlite3_open(agent_path.c_str(), &agent_db);
        if (rc != SQLITE_OK) {
            std::string msg = sqlite3_errmsg(agent_db);
            sqlite3_close(agent_db);
            throw std::runtime_error("Failed to create agent DB " + agent_path + ": " + msg);
        }
        sqlite3_close(agent_db);
    }

    // Write state.json
    save_manifest();

    is_new_ = true;
}

// -----------------------------------------------------------------------
// Integrity check
// -----------------------------------------------------------------------

bool AppSession::verify_integrity(bool force) {
    auto manifest_path = session_dir_ / "state.json";
    json manifest;
    {
        std::ifstream file(manifest_path);
        if (file.is_open()) {
            try {
                file >> manifest;
            } catch (...) {
            }
        }
    }

    // Get the list of files from the manifest
    std::set<std::string> listed;
    if (manifest.contains("files") && manifest["files"].is_array()) {
        for (const auto& f : manifest["files"]) {
            if (f.is_string()) listed.insert(f.get<std::string>());
        }
    }

    // Scan actual folder
    auto actual_set = std::set<std::string>();
    for (const auto& entry : std::filesystem::directory_iterator(session_dir_)) {
        if (entry.is_regular_file()) {
            std::string fname = entry.path().filename().string();
            if (fname != "state.json") {
                actual_set.insert(fname);
            }
        }
    }

    // Check: files listed in manifest but missing from disk
    std::vector<std::string> missing;
    for (const auto& f : listed) {
        if (actual_set.find(f) == actual_set.end()) {
            missing.push_back(f);
        }
    }

    // Check: files on disk but not listed in manifest
    std::vector<std::string> extra;
    for (const auto& f : actual_set) {
        if (listed.find(f) == listed.end()) {
            extra.push_back(f);
        }
    }

    bool has_issue = !missing.empty() || !extra.empty();

    if (has_issue && !force) {
        std::string msg = "Integrity check failed for session '" + session_name_ + "':\n";
        if (!missing.empty()) {
            msg += "  Missing files (listed in state.json but not on disk):\n";
            for (const auto& f : missing) {
                msg += "    - " + f + "\n";
            }
        }
        if (!extra.empty()) {
            msg += "  Extra files (on disk but not listed in state.json):\n";
            for (const auto& f : extra) {
                msg += "    - " + f + "\n";
            }
        }
        msg += "Use --force to bypass this check.";
        throw std::runtime_error(msg);
    }

    if (has_issue && force) {
        std::cerr << "Warning: integrity check issues for session '" << session_name_ << "':\n";
        if (!missing.empty()) {
            std::cerr << "  Missing files:";
            for (const auto& f : missing) {
                std::cerr << " " << f;
            }
            std::cerr << "\n";
        }
        if (!extra.empty()) {
            std::cerr << "  Extra files:";
            for (const auto& f : extra) {
                std::cerr << " " << f;
            }
            std::cerr << "\n";
        }
        std::cerr << "  Attempting to continue anyway.\n";
    }

    // Update manifest with actual file set for correct operation
    if (has_issue && force) {
        // Re-read manifest, update files list to match actual disk state
        if (!manifest.is_null()) {
            manifest["files"] = json::array();
            for (const auto& f : actual_set) {
                manifest["files"].push_back(f);
            }
            std::ofstream out(manifest_path);
            if (out.is_open()) {
                out << manifest.dump(2) << std::endl;
            }
        }
    }

    return has_issue;
}

// -----------------------------------------------------------------------
// Folder scan helper
// -----------------------------------------------------------------------

SessionFolderContents AppSession::scan_folder() const {
    SessionFolderContents contents;
    for (const auto& entry : std::filesystem::directory_iterator(session_dir_)) {
        if (entry.is_regular_file()) {
            std::string fname = entry.path().filename().string();
            if (fname != "state.json") {
                contents.filenames.push_back(fname);
            }
        }
    }
    return contents;
}

// -----------------------------------------------------------------------
// Path accessors
// -----------------------------------------------------------------------

std::string AppSession::wiki_db_path() const {
    return (session_dir_ / "wiki.db").string();
}

std::vector<std::string> AppSession::agent_db_filenames() const {
    return agent_dbs_;
}

std::string AppSession::agent_db_path(const std::string& filename) const {
    return (session_dir_ / filename).string();
}

// -----------------------------------------------------------------------
// Manifest data
// -----------------------------------------------------------------------

void AppSession::set_last_cwd(const std::string& cwd) {
    last_cwd_ = cwd;
}

// -----------------------------------------------------------------------
// Agent DB management
// -----------------------------------------------------------------------

void AppSession::add_agent_db(const std::string& filename) {
    // Avoid duplicates
    for (const auto& existing : agent_dbs_) {
        if (existing == filename) return;
    }
    agent_dbs_.push_back(filename);
    save_manifest();
}

void AppSession::remove_agent_db(const std::string& filename) {
    auto it = std::find(agent_dbs_.begin(), agent_dbs_.end(), filename);
    if (it != agent_dbs_.end()) {
        agent_dbs_.erase(it);
        save_manifest();
    }
}

// -----------------------------------------------------------------------
// Save manifest
// -----------------------------------------------------------------------

void AppSession::save_manifest() {
    json manifest;
    manifest["version"] = 1;
    manifest["last_cwd"] = last_cwd_;

    json files = json::array();
    files.push_back("wiki.db");
    for (const auto& ag : agent_dbs_) {
        files.push_back(ag);
        // Auxiliary files that share the same stem (chat log, plan file).
        // These are created alongside the agent DB; include them so the
        // integrity check doesn't flag them as extra.
        if (ag.size() >= 3 && ag.substr(ag.size() - 3) == ".db") {
            std::string stem = ag.substr(0, ag.size() - 3);
            files.push_back(stem + ".log");
            files.push_back(stem + ".plan.json");
            files.push_back(stem + ".messages.json");
        }
    }
    manifest["files"] = std::move(files);

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
                  << "  Session directory: " << session_dir_.string() << "\n"
                  << "  Agent tabs: " << agent_dbs_.size() << "\n";
    }
    std::cout << std::flush;
}
