#include "session.h"
#include "chat.h"

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

Session::Session(const std::string& name, ConfigPtr cfg, PlanBoardPtr plan)
    : session_name_{name}, cfg_{std::move(cfg)}, plan_{std::move(plan)} {
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

// ===================================================================
// Knob accessors
// ===================================================================

int Session::max_tool_iterations() const { return session_data_.max_tool_iterations; }
int Session::subagent_timeout() const { return session_data_.subagent_timeout; }
int Session::bash_timeout() const { return session_data_.bash_timeout; }
int Session::grep_timeout() const { return session_data_.grep_timeout; }
int Session::web_search_timeout() const { return session_data_.web_search_timeout; }
int Session::web_fetch_timeout() const { return session_data_.web_fetch_timeout; }

void Session::set_max_tool_iterations(int v) { session_data_.max_tool_iterations = v; }
void Session::set_subagent_timeout(int v) { session_data_.subagent_timeout = v; }
void Session::set_bash_timeout(int v) { session_data_.bash_timeout = v; }
void Session::set_grep_timeout(int v) { session_data_.grep_timeout = v; }
void Session::set_web_search_timeout(int v) { session_data_.web_search_timeout = v; }
void Session::set_web_fetch_timeout(int v) { session_data_.web_fetch_timeout = v; }

// ===================================================================
// apply_knobs_to — apply session knob overrides to a ChatSession
// ===================================================================

void Session::apply_knobs_to(ChatSession& session) const {
    const auto& cfg = *cfg_;
    // Effective values: knob override > 0 ? knob : cfg default
    const int mi = max_tool_iterations() > 0 ? max_tool_iterations() : kDefaultMaxToolIterations;
    const int sa = subagent_timeout() > 0 ? subagent_timeout() : kDefaultSubagentTimeout;
    const int bt = bash_timeout() > 0 ? bash_timeout() : kDefaultBashTimeout;
    const int gt = grep_timeout() > 0 ? grep_timeout() : kDefaultGrepTimeout;
    const int wst = web_search_timeout() > 0 ? web_search_timeout() : kDefaultWebSearchTimeout;
    const int wft = web_fetch_timeout() > 0 ? web_fetch_timeout() : kDefaultWebFetchTimeout;

    session.set_max_iterations(mi);

    // Rebuild time-sensitive tools with overridden timeouts.
    // We must remove the existing tools first, then re-add them with
    // the effective timeout values.
    // Note: we keep read-only-path-aware tools and plan tools intact.

    // Remove existing time-sensitive tools
    session.tools_for_testing().remove("run_bwrap");
    session.tools_for_testing().remove("run_bwrap_ro");
    session.tools_for_testing().remove("grep_files");
    session.tools_for_testing().remove("find_files");
    session.tools_for_testing().remove("web_search");
    session.tools_for_testing().remove("web_fetch");

    // Re-add with effective timeout values
    auto& tools = session.tools_for_testing();
    const auto& ro_paths = cfg.read_only_paths;
    auto safe_dir = std::make_shared<std::string>(session.safe_dir());
    auto cancelled = std::make_shared<std::atomic<bool>>(false); // placeholder

    // Re-add read-only bwrap
    {
        auto t = make_run_bwrap_tool(cfg, safe_dir, bt, cancelled, true);
        t.permission = ToolPermission::ReadOnly;
        tools.add(std::move(t));
    }
    // Re-add grep
    {
        auto t = make_grep_files_tool(cfg, safe_dir, ro_paths, gt, cancelled);
        t.permission = ToolPermission::ReadOnly;
        tools.add(std::move(t));
    }
    // Re-add find
    {
        auto t = make_find_files_tool(cfg, safe_dir, ro_paths, gt, cancelled);
        t.permission = ToolPermission::ReadOnly;
        tools.add(std::move(t));
    }
    // Re-add web_search
    {
        auto t = make_web_search_tool(cfg, wst, cancelled);
        t.permission = ToolPermission::ReadOnly;
        tools.add(std::move(t));
    }
    // Re-add web_fetch
    {
        auto t = make_web_fetch_tool(cfg, wft, cancelled);
        t.permission = ToolPermission::ReadOnly;
        tools.add(std::move(t));
    }
    // Re-add write bwrap (if session is not read-only)
    if (!session.is_read_only()) {
        auto t = make_run_bwrap_tool(cfg, safe_dir, bt, cancelled, /*read_only=*/false, /*allow_network=*/true);
        t.permission = ToolPermission::Write;
        tools.add(std::move(t));
    }
}
