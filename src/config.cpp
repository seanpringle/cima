#include "config.h"

#include <fstream>
#include <iostream>
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
    j["api_base"] = api_base;
    j["api_key"] = api_key;
    j["model"] = model;
    j["reasoning_effort"] = reasoning_effort;
    j["search_api_key"] = search_api_key;
    j["search_engine_id"] = search_engine_id;
    j["search_endpoint"] = search_endpoint;
    j["read_only_paths"] = read_only_paths;
    j["max_tool_iterations"] = max_tool_iterations;
    j["max_continuation_steps"] = max_continuation_steps;
    j["continuation_delay_ms"] = continuation_delay_ms;
    j["context_limit"] = context_limit;
    j["snippets"] = snippets;
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

        if (j.contains("api_base"))         cfg.api_base = j["api_base"].get<std::string>();
        if (j.contains("api_key"))          cfg.api_key = j["api_key"].get<std::string>();
        if (j.contains("model"))            cfg.model = j["model"].get<std::string>();
        if (j.contains("reasoning_effort")) cfg.reasoning_effort = j["reasoning_effort"].get<std::string>();
        if (j.contains("search_api_key"))   cfg.search_api_key = j["search_api_key"].get<std::string>();
        if (j.contains("search_engine_id")) cfg.search_engine_id = j["search_engine_id"].get<std::string>();
        if (j.contains("search_endpoint"))  cfg.search_endpoint = j["search_endpoint"].get<std::string>();

        if (j.contains("max_tool_iterations") && j["max_tool_iterations"].is_number_integer()) {
            int n = j["max_tool_iterations"].get<int>();
            if (n > 0) cfg.max_tool_iterations = n;
        }
        if (j.contains("max_continuation_steps") && j["max_continuation_steps"].is_number_integer()) {
            int n = j["max_continuation_steps"].get<int>();
            if (n >= 0) cfg.max_continuation_steps = n;
        }
        if (j.contains("continuation_delay_ms") && j["continuation_delay_ms"].is_number_integer()) {
            int n = j["continuation_delay_ms"].get<int>();
            if (n >= 0) cfg.continuation_delay_ms = n;
        }
        if (j.contains("context_limit") && j["context_limit"].is_number_integer()) {
            int n = j["context_limit"].get<int>();
            if (n > 0) cfg.context_limit = n;
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
