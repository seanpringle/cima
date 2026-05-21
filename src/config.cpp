#include "config.h"

#include <fstream>
#include <iostream>
#include <set>
#include <string>

Config cfg;

std::string Config::SYSTEM_PROMPT =
    "You are an AI coding assistant.\n"
    "Use markdown with a neat, clear and concise layout for all output.\n"
    "All of commonmark and github tables supported, but generally prefer lists over tables.\n"
    "\n"
    "## Sub Agents\n"
    "\n"
    "Call subagents with `call_subagent()`. See tool description for available subagents.\n"
    "\n"
    "## Plan tools\n"
    "\n"
    "You have a **Plan document** shared with the user. When given a task, research"
    " it thoroughly and write your Plan with `write_plan()`."
    " Ask the user to review and approve your Plan before implementation.\n"
    "Go back and check your Plan at any time with `read_plan()`.\n"
    "\n"
    "## Long tool output\n"
    "\n"
    "Tool output > 100 lines or 4K chars may be placed into the tool log."
    " The output will be a message with a log entry ID."
    " Use view_tool_output(ID=?,head=N,tail=N) to retrieve it.\n"
    "\n";

std::string Config::SUBAGENT_SYSTEM_PROMPT =
    "You are an AI coding assistant working as a subagent.\n"
    "Use markdown with a neat, clear and concise layout for all output.\n"
    "All of commonmark and github tables supported, but generally prefer lists over tables.\n";

std::string Config::CMAKE_PROMPT_SNIPPET =
    "## CMake tools\n"
    "`cmake_configure()` configures the project (generates compile_commands.json).\n"
    "`cmake_build()` builds the project.\n"
    "`cmake_ctest()` runs the test suite.\n"
    "All return raw output with optional head/tail trimming.\n";

// ---------------------------------------------------------------------------
// Config file path
// ---------------------------------------------------------------------------

std::filesystem::path Config::config_file_path() {
    const char* home = std::getenv("HOME");
    if (!home || !home[0]) {
        home = std::getenv("USERPROFILE");
    }
    if (!home || !home[0]) {
        throw std::runtime_error("Cannot determine home directory (HOME not set)");
    }
    return std::filesystem::path(home) / ".config" / "cima" / "cima.json";
}

// ---------------------------------------------------------------------------
// Serialise JSON-persisted fields (excludes system_prompt)
// ---------------------------------------------------------------------------

json Config::to_json() const {
    json j;
    json prov_arr = json::array();
    for (const auto& p : providers) {
        json pj;
        pj["name"] = p.name;
        pj["api_base"] = p.api_base;
        pj["api_key"] = p.api_key;
        pj["model"] = p.model;
        pj["reasoning_effort"] = p.reasoning_effort;
        pj["reasoning_efforts"] = p.reasoning_efforts;
        pj["context_limit"] = p.context_limit;
        prov_arr.push_back(std::move(pj));
    }
    j["providers"] = std::move(prov_arr);

    // ── MCP servers ──
    json mcp_arr = json::array();
    for (const auto& m : mcp_servers) {
        json mj;
        mj["name"] = m.name;
        mj["transport"] = m.transport;
        mj["command"] = m.command;
        mj["args"] = m.args;
        mj["cwd"] = m.cwd;
        mj["url"] = m.url;
        mj["api_key"] = m.api_key;
        mj["env"] = m.env;
        mj["timeout_sec"] = m.timeout_sec;
        mcp_arr.push_back(std::move(mj));
    }
    j["mcp_servers"] = std::move(mcp_arr);

    j["read_only_paths"] = read_only_paths;
    j["max_tool_iterations"] = max_tool_iterations;
    j["snippets"] = snippets;
    j["subagent_timeout"] = subagent_timeout;
    j["bash_timeout"] = bash_timeout;
    j["cmake_enabled"] = cmake_enabled;
    j["cmake_configure_timeout"] = cmake_configure_timeout;
    j["cmake_build_timeout"] = cmake_build_timeout;
    j["cmake_ctest_timeout"] = cmake_ctest_timeout;
    j["project_tree_timeout"] = project_tree_timeout;
    j["git_status_timeout"] = git_status_timeout;
    j["git_diff_timeout"] = git_diff_timeout;
    j["git_log_timeout"] = git_log_timeout;
    j["git_add_timeout"] = git_add_timeout;
    j["git_commit_timeout"] = git_commit_timeout;
    j["grep_timeout"] = grep_timeout;
    j["web_search_timeout"] = web_search_timeout;
    j["web_fetch_timeout"] = web_fetch_timeout;
    // ── Subagents ──
    json sa_arr = json::array();
    for (const auto& sa : subagents) {
        json saj;
        saj["name"] = sa.name;
        saj["description"] = sa.description;
        saj["read_only"] = sa.read_only;
        sa_arr.push_back(std::move(saj));
    }
    j["subagents"] = std::move(sa_arr);

    // ── cmd_tools ──
    json ct_arr = json::array();
    for (const auto& ct : cmd_tools) {
        json ctj;
        ctj["name"] = ct.name;
        ctj["description"] = ct.description;
        ctj["command"] = ct.command;
        ct_arr.push_back(std::move(ctj));
    }
    j["cmd_tools"] = std::move(ct_arr);

    return j;
}

// ---------------------------------------------------------------------------
// Load snippet files from ~/.config/cima/snippets/*.md
// ---------------------------------------------------------------------------

static void load_snippet_files(std::map<std::string, std::string>& snippets) {
    auto dir = Config::config_file_path().parent_path() / "snippets";
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return; // silently skip if directory doesn't exist
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file())
            continue;
        auto path = entry.path();
        if (path.extension() != ".md")
            continue;

        std::string name = path.stem().string();
        if (name.empty())
            continue;

        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Warning: cannot read snippet file: " << path.string() << std::endl;
            continue;
        }
        std::string content(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        snippets[name] = std::move(content);
    }
}

// ---------------------------------------------------------------------------
// Load config from file
// ---------------------------------------------------------------------------

Config Config::load() {
    Config cfg;

    auto path = config_file_path();
    std::ifstream file(path);
    if (file.is_open()) {
        json j;
        try {
            file >> j;
        } catch (...) {
            std::cerr << "Warning: " << path.string()
                      << " is corrupt or unreadable — proceeding with defaults\n";
        }

        // ── Parse providers array ──
        if (j.contains("providers") && j["providers"].is_array()) {
            for (const auto& pj : j["providers"]) {
                Provider p;
                p.name = pj.value("name", std::string());
                p.api_base = pj.value("api_base", std::string());
                p.api_key = pj.value("api_key", std::string());
                p.model = pj.value("model", std::string());
                p.reasoning_effort = pj.value("reasoning_effort", std::string());
                p.reasoning_efforts = pj.value("reasoning_efforts", std::vector<std::string>());
                p.context_limit = pj.value("context_limit", 300000);
                cfg.providers.push_back(std::move(p));
            }
        }

        // ── Validate provider name uniqueness ──
        std::set<std::string> seen;
        std::vector<std::string> duplicates;
        for (const auto& p : cfg.providers) {
            if (!seen.insert(p.name).second) {
                duplicates.push_back(p.name);
            }
        }
        if (!duplicates.empty()) {
            std::string msg = "Duplicate provider name(s) in cima.json: ";
            for (size_t i = 0; i < duplicates.size(); i++) {
                if (i > 0)
                    msg += ", ";
                msg += "\"" + duplicates[i] + "\"";
            }
            throw std::runtime_error(msg);
        }
        if (cfg.providers.empty()) {
            throw std::runtime_error(
                "cima.json must contain at least one provider in the \"providers\" array");
        }

        // ── Parse MCP servers array ──
        if (j.contains("mcp_servers") && j["mcp_servers"].is_array()) {
            for (const auto& mj : j["mcp_servers"]) {
                McpEndpoint m;
                m.name = mj.value("name", std::string());
                m.transport = mj.value("transport", std::string("stdio"));
                m.command = mj.value("command", std::string());
                m.url = mj.value("url", std::string());
                m.api_key = mj.value("api_key", std::string());
                m.cwd = mj.value("cwd", std::string());
                m.timeout_sec = mj.value("timeout_sec", 60);

                // Parse args array
                if (mj.contains("args") && mj["args"].is_array()) {
                    for (const auto& a : mj["args"]) {
                        if (a.is_string()) {
                            m.args.push_back(a.get<std::string>());
                        }
                    }
                }

                // Parse env map
                if (mj.contains("env") && mj["env"].is_object()) {
                    for (auto it = mj["env"].begin(); it != mj["env"].end(); ++it) {
                        if (it.value().is_string()) {
                            m.env[it.key()] = it.value().get<std::string>();
                        }
                    }
                }

                cfg.mcp_servers.push_back(std::move(m));
            }
        }

        // ── Parse subagents array ──
        if (j.contains("subagents") && j["subagents"].is_array()) {
            for (const auto& saj : j["subagents"]) {
                SubagentConfig sa;
                sa.name = saj.value("name", std::string());
                sa.description = saj.value("description", std::string());
                sa.read_only = saj.value("read_only", false);
                if (!sa.name.empty()) {
                    cfg.subagents.push_back(std::move(sa));
                }
            }
        }

        // ── Parse cmd_tools array ──
        if (j.contains("cmd_tools") && j["cmd_tools"].is_array()) {
            for (const auto& ctj : j["cmd_tools"]) {
                CmdToolConfig ct;
                ct.name = ctj.value("name", std::string());
                ct.description = ctj.value("description", std::string());
                ct.command = ctj.value("command", std::string());
                if (!ct.name.empty() && !ct.command.empty()) {
                    cfg.cmd_tools.push_back(std::move(ct));
                }
            }
        }

        // ── Other top-level fields ──
        if (j.contains("max_tool_iterations") && j["max_tool_iterations"].is_number_integer()) {
            int n = j["max_tool_iterations"].get<int>();
            if (n > 0)
                cfg.max_tool_iterations = n;
        }

        // Tool timeouts (int, >= 0)
        auto load_timeout = [&](const std::string& key, int& field) {
            if (j.contains(key) && j[key].is_number_integer()) {
                int n = j[key].get<int>();
                if (n >= 0)
                    field = n;
            }
        };
        load_timeout("subagent_timeout", cfg.subagent_timeout);
        load_timeout("bash_timeout", cfg.bash_timeout);
        if (j.contains("cmake_enabled") && j["cmake_enabled"].is_boolean())
            cfg.cmake_enabled = j["cmake_enabled"].get<bool>();
        load_timeout("cmake_configure_timeout", cfg.cmake_configure_timeout);
        load_timeout("cmake_build_timeout", cfg.cmake_build_timeout);
        load_timeout("cmake_ctest_timeout", cfg.cmake_ctest_timeout);
        load_timeout("project_tree_timeout", cfg.project_tree_timeout);
        load_timeout("git_status_timeout", cfg.git_status_timeout);
        load_timeout("git_diff_timeout", cfg.git_diff_timeout);
        load_timeout("git_log_timeout", cfg.git_log_timeout);
        load_timeout("git_add_timeout", cfg.git_add_timeout);
        load_timeout("git_commit_timeout", cfg.git_commit_timeout);
        load_timeout("grep_timeout", cfg.grep_timeout);
        load_timeout("web_search_timeout", cfg.web_search_timeout);
        load_timeout("web_fetch_timeout", cfg.web_fetch_timeout);

        // Font settings
        if (j.contains("font_sans") && j["font_sans"].is_string()) {
            cfg.font_sans = j["font_sans"].get<std::string>();
        }
        if (j.contains("font_mono") && j["font_mono"].is_string()) {
            cfg.font_mono = j["font_mono"].get<std::string>();
        }
        if (j.contains("font_size") && j["font_size"].is_number_integer()) {
            int n = j["font_size"].get<int>();
            if (n > 0)
                cfg.font_size = n;
        }

        if (j.contains("read_only_paths") && j["read_only_paths"].is_array()) {
            cfg.read_only_paths.clear();
            for (const auto& p : j["read_only_paths"]) {
                if (p.is_string()) {
                    cfg.read_only_paths.push_back(p.get<std::string>());
                }
            }
        }

        // Load snippets from config (static, user-edited file)
        if (j.contains("snippets") && j["snippets"].is_object()) {
            for (auto it = j["snippets"].begin(); it != j["snippets"].end(); ++it) {
                if (it.value().is_string()) {
                    cfg.snippets[it.key()] = it.value().get<std::string>();
                }
            }
        }
    } else {
        // File doesn't exist — create directory and write defaults
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!ec) {
            // Create a default provider so the config is valid
            Provider def;
            def.name = "default";
            def.api_base = "http://127.0.0.1:11000/v1";
            def.model = "deepseek-v4-flash";
            def.context_limit = 300000;
            cfg.providers.push_back(std::move(def));

            std::ofstream out(path);
            if (out.is_open()) {
                out << cfg.to_json().dump(2) << std::endl;
            }
        }
    }

    // Default read-only paths — ensure they're present even if JSON omitted them
    bool has_usr_include = false, has_usr_doc = false;
    for (const auto& p : cfg.read_only_paths) {
        if (p == "/usr/include")
            has_usr_include = true;
        if (p == "/usr/share/doc")
            has_usr_doc = true;
    }
    if (!has_usr_include)
        cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/include");
    if (!has_usr_doc)
        cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/share/doc");

    // Load file-based snippets from ~/.config/cima/snippets/*.md
    // These override any snippets from cima.json on name collision.
    load_snippet_files(cfg.snippets);

    return cfg;
}
