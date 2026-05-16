#include "config.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <string>

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
        pj["context_limit"] = p.context_limit;
        prov_arr.push_back(std::move(pj));
    }
    j["providers"] = std::move(prov_arr);
    j["read_only_paths"] = read_only_paths;
    j["max_tool_iterations"] = max_tool_iterations;
    j["snippets"] = snippets;
    j["bash_timeout"] = bash_timeout;
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
    j["clangd_path"] = clangd_path;
    j["clangd_args"] = clangd_args;
    j["lsp_timeout"] = lsp_timeout;
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
        if (!entry.is_regular_file()) continue;
        auto path = entry.path();
        if (path.extension() != ".md") continue;

        std::string name = path.stem().string();
        if (name.empty()) continue;

        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Warning: cannot read snippet file: " << path.string() << std::endl;
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
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
            // Corrupt file — proceed with defaults
        }

        // ── Parse providers array ──
        if (j.contains("providers") && j["providers"].is_array()) {
            for (const auto& pj : j["providers"]) {
                Provider p;
                p.name = pj.value("name", std::string());
                p.api_base = pj.value("api_base", std::string());
                p.api_key = pj.value("api_key", std::string());
                p.model = pj.value("model", std::string());
                p.reasoning_effort = pj.value("reasoning_effort", std::string("high"));
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
                if (i > 0) msg += ", ";
                msg += "\"" + duplicates[i] + "\"";
            }
            throw std::runtime_error(msg);
        }
        if (cfg.providers.empty()) {
            throw std::runtime_error("cima.json must contain at least one provider in the \"providers\" array");
        }

        // ── Other top-level fields ──
        if (j.contains("max_tool_iterations") && j["max_tool_iterations"].is_number_integer()) {
            int n = j["max_tool_iterations"].get<int>();
            if (n > 0) cfg.max_tool_iterations = n;
        }

        // Tool timeouts (int, >= 0)
        auto load_timeout = [&](const std::string& key, int& field) {
            if (j.contains(key) && j[key].is_number_integer()) {
                int n = j[key].get<int>();
                if (n >= 0) field = n;
            }
        };
        load_timeout("bash_timeout", cfg.bash_timeout);
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

        // LSP settings
        if (j.contains("clangd_path") && j["clangd_path"].is_string()) {
            cfg.clangd_path = j["clangd_path"].get<std::string>();
        }
        if (j.contains("clangd_args") && j["clangd_args"].is_array()) {
            cfg.clangd_args.clear();
            for (const auto& a : j["clangd_args"]) {
                if (a.is_string()) {
                    cfg.clangd_args.push_back(a.get<std::string>());
                }
            }
        }
        if (j.contains("lsp_timeout") && j["lsp_timeout"].is_number_integer()) {
            int n = j["lsp_timeout"].get<int>();
            if (n >= 0) cfg.lsp_timeout = n;
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
            def.reasoning_effort = "high";
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
        if (p == "/usr/include")   has_usr_include = true;
        if (p == "/usr/share/doc") has_usr_doc = true;
    }
    if (!has_usr_include) cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/include");
    if (!has_usr_doc)     cfg.read_only_paths.insert(cfg.read_only_paths.begin(), "/usr/share/doc");

    // Load file-based snippets from ~/.config/cima/snippets/*.md
    // These override any snippets from cima.json on name collision.
    load_snippet_files(cfg.snippets);

    return cfg;
}
