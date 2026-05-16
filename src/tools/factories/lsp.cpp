#include "tools.h"
#include "lsp/json_rpc.h"
#include "lsp/lsp_client.h"

#include <algorithm>
#include <fstream>
#include <string>

// ===================================================================
// get_lsp_diagnostics
// ===================================================================

Tool make_get_lsp_diagnostics_tool(LspClient& lsp) {
    Tool t;
    t.name = "get_lsp_diagnostics";
    t.description =
        "Get compiler errors and warnings for a file using the LSP "
        "(clangd) language server.\n"
        "The file must already exist on disk. Returns diagnostics "
        "with severity, message, file, line, and column.\n"
        "Requires 'lsp_enabled: true' in cima.json and clangd installed.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file to check"}}}}},
        {"required", {"path"}}};
    t.execute = [&lsp](const json& args) -> Result<std::string> {
        if (!lsp.is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Enable with \"lsp_enabled\": true in cima.json "
                            "and ensure clangd is installed."));
        }

        auto raw_path = args.value("path", std::string());
        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }

        // Resolve and read the file content for sync
        // Use resolve_path to check the path is valid (but we don't have
        // safe_dir here — the caller should resolve first, or we accept
        // any path and let the LSP server handle it).
        // For now, we just pass the path through to the LSP server.
        // The path sandbox is enforced by the caller (ChatSession).
        std::string uri = lsp::path_to_uri(raw_path);

        // Read the file content for syncing
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(size, '\0');
        file.seekg(0);
        file.read(content.data(), size);

        // Sync the file with the LSP server
        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp.ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                std::string("Failed to sync file with LSP server: ") +
                sync.error());
        }

        // Request diagnostics
        auto resp = lsp.request("textDocument/pullDiagnostics",
            {{"textDocument", {{"uri", uri}}}},
            15);

        if (!resp) {
            // Check if the error is MethodNotFound (older clangd)
            if (resp.error().find("MethodNotFound") != std::string::npos ||
                resp.error().find("method not found") != std::string::npos) {
                return std::unexpected(
                    std::string("clangd version too old — LSP 3.17 pullDiagnostics "
                                "requires clangd >= 14.0.0. "
                                "Please upgrade your clangd installation."));
            }
            return std::unexpected(resp.error());
        }

        auto& response = *resp;

        // Check for JSON-RPC error response
        if (response.contains("error")) {
            auto& err = response["error"];
            int code = err.value("code", 0);
            std::string msg = err.value("message", "unknown error");
            if (code == lsp::ErrorCodes::MethodNotFound) {
                return std::unexpected(
                    std::string("LSP method not found (clangd too old?): ") + msg);
            }
            return std::unexpected(
                std::string("LSP error [") + std::to_string(code) +
                "]: " + msg);
        }

        // Extract diagnostics from result
        if (!response.contains("result")) {
            return std::unexpected(
                std::string("LSP response missing 'result' field"));
        }

        auto& result = response["result"];

        // Build formatted output
        std::string output;
        int total_count = 0;

        // Diagnostics for the primary document
        if (result.contains("diagnostics") && result["diagnostics"].is_array()) {
            auto& diags = result["diagnostics"];
            for (const auto& d : diags) {
                total_count++;
                std::string severity_str;
                int severity = d.value("severity", 0);
                switch (severity) {
                    case 1: severity_str = "error"; break;
                    case 2: severity_str = "warning"; break;
                    case 3: severity_str = "info"; break;
                    case 4: severity_str = "hint"; break;
                    default: severity_str = "note"; break;
                }

                auto& range = d["range"];
                auto& start = range["start"];
                int line = start.value("line", 0);
                int col = start.value("character", 0);

                output += "- [" + severity_str + "] " +
                          raw_path + ":" +
                          std::to_string(line + 1) + ":" +
                          std::to_string(col + 1) + ": " +
                          d.value("message", "") + "\n";

                // Include code if present
                if (d.contains("code") && !d["code"].is_null()) {
                    if (d["code"].is_string()) {
                        output += "  code: " + d["code"].get<std::string>() + "\n";
                    } else if (d["code"].is_number()) {
                        output += "  code: " + std::to_string(d["code"].get<int>()) + "\n";
                    }
                }
            }
        }

        // Diagnostics from related documents (cross-file issues)
        if (result.contains("relatedDocuments") &&
            result["relatedDocuments"].is_object()) {
            for (auto it = result["relatedDocuments"].begin();
                 it != result["relatedDocuments"].end(); ++it) {
                const auto& related_uri = it.key();
                auto& related_diags = it.value();
                if (!related_diags.is_array())
                    continue;

                // Convert URI to path for display
                auto related_path = lsp::uri_to_path(related_uri);
                const std::string& display_path =
                    related_path ? *related_path : related_uri;

                for (const auto& d : related_diags) {
                    total_count++;
                    std::string severity_str;
                    int severity = d.value("severity", 0);
                    switch (severity) {
                        case 1: severity_str = "error"; break;
                        case 2: severity_str = "warning"; break;
                        case 3: severity_str = "info"; break;
                        case 4: severity_str = "hint"; break;
                        default: severity_str = "note"; break;
                    }

                    auto& range = d["range"];
                    auto& start = range["start"];
                    int line = start.value("line", 0);
                    int col = start.value("character", 0);

                    output += "- [" + severity_str + "] " +
                              display_path + ":" +
                              std::to_string(line + 1) + ":" +
                              std::to_string(col + 1) + ": " +
                              d.value("message", "") + "\n";

                    if (d.contains("code") && !d["code"].is_null()) {
                        if (d["code"].is_string()) {
                            output += "  code: " + d["code"].get<std::string>() + "\n";
                        } else if (d["code"].is_number()) {
                            output += "  code: " + std::to_string(d["code"].get<int>()) + "\n";
                        }
                    }
                }
            }
        }

        if (total_count == 0) {
            return std::string("(no diagnostics)");
        }

        // Prepend count
        output = std::to_string(total_count) +
                 (total_count == 1 ? " diagnostic" : " diagnostics") +
                 ":\n" + output;
        return output;
    };
    return t;
}

// ---------------------------------------------------------------------------
// Helpers for formatting LSP responses
// ---------------------------------------------------------------------------

/// Append formatted hover contents to `output`.
/// LSP hover `contents` can be a string, a MarkupContent, a MarkedString,
/// or an array of MarkedStrings.
static void append_hover_contents(std::string& output, const json& contents,
                                   const std::string& filename) {
    if (contents.is_string()) {
        output += contents.get<std::string>() + "\n";
    } else if (contents.is_array()) {
        // Array of MarkedStrings
        for (const auto& item : contents) {
            append_hover_contents(output, item, filename);
        }
    } else if (contents.is_object()) {
        if (contents.contains("kind") && contents.contains("value")) {
            // MarkupContent: {kind: "markdown"|"plaintext", value: "..."}
            auto kind = contents["kind"].get<std::string>();
            auto value = contents["value"].get<std::string>();
            if (kind == "markdown") {
                // Pass through raw markdown
                output += value + "\n";
            } else {
                // Plaintext — wrap in a code block
                auto lang = LspClient::language_id_from_extension(filename);
                output += "```" + lang + "\n" + value + "\n```\n";
            }
        } else if (contents.contains("language") && contents.contains("value")) {
            // MarkedString: {language: "cpp", value: "..."}
            auto lang = contents["language"].get<std::string>();
            auto value = contents["value"].get<std::string>();
            output += "```" + lang + "\n" + value + "\n```\n";
        }
    }
}

/// Append a formatted location to `output`.
static void format_location(std::string& output, const json& loc) {
    std::string path;
    if (loc.contains("uri")) {
        auto p = lsp::uri_to_path(loc["uri"].get<std::string>());
        path = p.value_or(loc["uri"].get<std::string>());
    } else {
        path = "(unknown)";
    }

    int line = 0, col = 0;
    if (loc.contains("range") && loc["range"].contains("start")) {
        line = loc["range"]["start"].value("line", 0);
        col = loc["range"]["start"].value("character", 0);
    }

    output += path + ":" + std::to_string(line + 1) + ":" +
              std::to_string(col + 1);
}

// ===================================================================
// get_lsp_hover
// ===================================================================

Tool make_get_lsp_hover_tool(LspClient& lsp) {
    Tool t;
    t.name = "get_lsp_hover";
    t.description =
        "Show type information and documentation for a symbol at a "
        "given file position.\n"
        "Uses the LSP (clangd) language server to query hover info.\n"
        "Returns the type signature and any doc comments.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file"}}},
             {"line",
                {{"type", "integer"},
                    {"description", "0-based line number"}}},
             {"character",
                {{"type", "integer"},
                    {"description", "0-based column offset (in UTF-16 code units)"}}}}},
        {"required", {"path", "line", "character"}}};
    t.execute = [&lsp](const json& args) -> Result<std::string> {
        if (!lsp.is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Enable with \"lsp_enabled\": true in cima.json "
                            "and ensure clangd is installed."));
        }

        auto raw_path = args.value("path", std::string());
        auto line = args.value("line", -1);
        auto character = args.value("character", -1);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0) {
            return std::unexpected(std::string("line must be >= 0"));
        }
        if (character < 0) {
            return std::unexpected(std::string("character must be >= 0"));
        }

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(size, '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp.ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request hover
        auto resp = lsp.request("textDocument/hover", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}
        }, 10);

        if (!resp) {
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            return std::unexpected(
                std::string("LSP error [") +
                std::to_string(err.value("code", 0)) + "]: " +
                err.value("message", ""));
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(no info)");
        }

        auto& result = response["result"];

        // Format the hover response
        // The LSP hover result has a `contents` field which can be:
        //   - A MarkupContent: {kind: "markdown"|"plaintext", value: "..."}
        //   - A MarkedString: {language: "cpp", value: "..."}
        //   - A plain string: "type info"
        //   - An array of MarkedStrings
        std::string output;

        if (result.contains("range") && !result["range"].is_null()) {
            auto& range = result["range"];
            auto& start = range["start"];
            output += "Range: [" +
                      std::to_string(start["line"].get<int>() + 1) + ":" +
                      std::to_string(start["character"].get<int>() + 1) + " - ";
            auto& end = range["end"];
            output += std::to_string(end["line"].get<int>() + 1) + ":" +
                      std::to_string(end["character"].get<int>() + 1) + "]\n";
        }

        append_hover_contents(output, result["contents"], raw_path);

        if (output.empty()) {
            return std::string("(no info)");
        }
        return output;
    };
    return t;
}

// ===================================================================
// get_lsp_definition
// ===================================================================

Tool make_get_lsp_definition_tool(LspClient& lsp) {
    Tool t;
    t.name = "get_lsp_definition";
    t.description =
        "Find the definition location of a symbol at a given file "
        "position.\n"
        "Uses the LSP (clangd) language server.\n"
        "Returns the file path and line:column of the definition.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file"}}},
             {"line",
                {{"type", "integer"},
                    {"description", "0-based line number"}}},
             {"character",
                {{"type", "integer"},
                    {"description", "0-based column offset (in UTF-16 code units)"}}}}},
        {"required", {"path", "line", "character"}}};
    t.execute = [&lsp](const json& args) -> Result<std::string> {
        if (!lsp.is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Enable with \"lsp_enabled\": true in cima.json "
                            "and ensure clangd is installed."));
        }

        auto raw_path = args.value("path", std::string());
        auto line = args.value("line", -1);
        auto character = args.value("character", -1);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0) {
            return std::unexpected(std::string("line must be >= 0"));
        }
        if (character < 0) {
            return std::unexpected(std::string("character must be >= 0"));
        }

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(size, '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp.ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request definition
        auto resp = lsp.request("textDocument/definition", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}
        }, 10);

        if (!resp) {
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            return std::unexpected(
                std::string("LSP error [") +
                std::to_string(err.value("code", 0)) + "]: " +
                err.value("message", ""));
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(no definition found)");
        }

        auto& result = response["result"];

        // The definition can be a single Location or an array of Location
        // (for overloaded functions/templates).
        // Normalize to an array for uniform handling.
        json locations;
        if (result.is_array()) {
            locations = result;
        } else if (result.is_object()) {
            locations = json::array({result});
        } else {
            return std::string("(no definition found)");
        }

        if (locations.empty()) {
            return std::string("(no definition found)");
        }

        std::string output;
        if (locations.size() == 1) {
            output = "Symbol defined at ";
            format_location(output, locations[0]);
        } else {
            output = std::to_string(locations.size()) + " definitions:\n";
            for (size_t i = 0; i < locations.size(); i++) {
                output += std::to_string(i + 1) + ". ";
                format_location(output, locations[i]);
                output += "\n";
            }
            // Remove trailing newline
            if (!output.empty() && output.back() == '\n')
                output.pop_back();
        }
        return output;
    };
    return t;
}

/// Map LSP CompletionItemKind integer to a human-readable name.
static std::string completion_kind_name(int kind) {
    switch (kind) {
        case 1:  return "Text";
        case 2:  return "Method";
        case 3:  return "Function";
        case 4:  return "Constructor";
        case 5:  return "Field";
        case 6:  return "Variable";
        case 7:  return "Class";
        case 8:  return "Interface";
        case 9:  return "Module";
        case 10: return "Property";
        case 11: return "Unit";
        case 12: return "Value";
        case 13: return "Enum";
        case 14: return "Keyword";
        case 15: return "Snippet";
        case 16: return "Color";
        case 17: return "File";
        case 18: return "Reference";
        case 19: return "Folder";
        case 20: return "EnumMember";
        case 21: return "Constant";
        case 22: return "Struct";
        case 23: return "Event";
        case 24: return "Operator";
        case 25: return "TypeParameter";
        default: return "";
    }
}

// ===================================================================
// get_lsp_completion
// ===================================================================

Tool make_get_lsp_completion_tool(LspClient& lsp) {
    Tool t;
    t.name = "get_lsp_completion";
    t.description =
        "Get code completion suggestions at a given position.\n"
        "Uses the LSP (clangd) language server.\n"
        "Returns a list of possible completions with labels, kinds, "
        "and signatures.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file"}}},
             {"line",
                {{"type", "integer"},
                    {"description", "0-based line number"}}},
             {"character",
                {{"type", "integer"},
                    {"description", "0-based column offset"}}},
             {"max_items",
                {{"type", "integer"},
                    {"description", "Maximum number of items to return (default 20)"}}}}},
        {"required", {"path", "line", "character"}}};
    t.execute = [&lsp](const json& args) -> Result<std::string> {
        if (!lsp.is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Enable with \"lsp_enabled\": true in cima.json "
                            "and ensure clangd is installed."));
        }

        auto raw_path = args.value("path", std::string());
        auto line = args.value("line", -1);
        auto character = args.value("character", -1);
        int max_items = args.value("max_items", 20);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0 || character < 0) {
            return std::unexpected(
                std::string("line and character must be >= 0"));
        }
        if (max_items < 1) max_items = 1;

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(size, '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp.ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request completion
        auto resp = lsp.request("textDocument/completion", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}
        }, 10);

        if (!resp) {
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            return std::unexpected(
                std::string("LSP error [") +
                std::to_string(err.value("code", 0)) + "]: " +
                err.value("message", ""));
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(no completions)");
        }

        auto& result = response["result"];

        // Result can be a CompletionList or an array of CompletionItem
        json items;
        bool is_incomplete = false;
        if (result.is_object() && result.contains("items")) {
            // CompletionList
            items = result["items"];
            is_incomplete = result.value("isIncomplete", false);
        } else if (result.is_array()) {
            items = result;
        } else {
            return std::string("(no completions)");
        }

        if (items.empty()) {
            return std::string("(no completions)");
        }

        // Apply max_items limit
        int total = static_cast<int>(items.size());
        int shown = std::min(total, max_items);
        int remaining = total - shown;

        // Build output
        std::string output = std::to_string(shown) + " completions";
        if (is_incomplete) {
            output += " (list may be incomplete)";
        }
        output += ":\n";

        for (int i = 0; i < shown; i++) {
            const auto& item = items[i];
            std::string label = item.value("label", "(unnamed)");
            std::string detail = item.value("detail", "");
            std::string kind_str = completion_kind_name(item.value("kind", 0));

            output += std::to_string(i + 1) + ". `" + label + "`";
            if (!kind_str.empty()) {
                output += " (" + kind_str + ")";
            }
            if (!detail.empty()) {
                output += " — " + detail;
            }
            output += "\n";

            // Show documentation on the next line if present (short version)
            if (item.contains("documentation") && !item["documentation"].is_null()) {
                std::string doc;
                const auto& docs = item["documentation"];
                if (docs.is_string()) {
                    doc = docs.get<std::string>();
                } else if (docs.is_object() && docs.contains("value")) {
                    doc = docs["value"].get<std::string>();
                }
                // Truncate long docs
                if (doc.size() > 120) {
                    doc = doc.substr(0, 120) + "...";
                }
                if (!doc.empty()) {
                    // Add indented doc line
                    output += "   " + doc + "\n";
                }
            }
        }

        if (remaining > 0) {
            output += "(" + std::to_string(remaining) + " more";
            if (is_incomplete) {
                output += " or more";
            }
            output += "...)\n";
        }

        return output;
    };
    return t;
}

// ===================================================================
// get_lsp_code_actions
// ===================================================================

Tool make_get_lsp_code_actions_tool(LspClient& lsp) {
    Tool t;
    t.name = "get_lsp_code_actions";
    t.description =
        "List available code actions (fixes, refactors, etc.) at a "
        "given position.\n"
        "Uses the LSP (clangd) language server.\n"
        "First queries diagnostics, then code actions for the context.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file"}}},
             {"line",
                {{"type", "integer"},
                    {"description", "0-based line number"}}},
             {"character",
                {{"type", "integer"},
                    {"description", "0-based column offset"}}},
             {"diagnostic_index",
                {{"type", "integer"},
                    {"description", "If set, scope actions to the diagnostic at this index (0-based)"}}}}},
        {"required", {"path", "line", "character"}}};
    t.execute = [&lsp](const json& args) -> Result<std::string> {
        if (!lsp.is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Enable with \"lsp_enabled\": true in cima.json "
                            "and ensure clangd is installed."));
        }

        auto raw_path = args.value("path", std::string());
        auto line = args.value("line", -1);
        auto character = args.value("character", -1);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0 || character < 0) {
            return std::unexpected(
                std::string("line and character must be >= 0"));
        }

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(size, '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp.ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // First, get diagnostics to provide context for code actions
        auto diag_resp = lsp.request("textDocument/pullDiagnostics", {
            {"textDocument", {{"uri", uri}}}
        }, 10);

        if (!diag_resp || (*diag_resp).contains("error")) {
            // If diagnostics fail, still try code actions at the position
            // without diagnostic context
        }

        // Build the codeAction context from diagnostics
        json context = {{"diagnostics", json::array()}};

        if (diag_resp && diag_resp->contains("result") &&
            !(*diag_resp)["result"].is_null()) {
            auto& result = (*diag_resp)["result"];
            auto diag_index = args.value("diagnostic_index", -1);

            if (result.contains("diagnostics") && result["diagnostics"].is_array()) {
                auto& diags = result["diagnostics"];

                if (diag_index >= 0 && diag_index < static_cast<int>(diags.size())) {
                    // Scope to a single diagnostic
                    context["diagnostics"] = json::array({diags[diag_index]});
                } else {
                    context["diagnostics"] = diags;
                }
            }
        }

        // Request code actions
        auto resp = lsp.request("textDocument/codeAction", {
            {"textDocument", {{"uri", uri}}},
            {"range", {
                {"start", {{"line", line}, {"character", character}}},
                {"end", {{"line", line}, {"character", character + 1}}}
            }},
            {"context", context}
        }, 10);

        if (!resp) {
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            return std::unexpected(
                std::string("LSP error [") +
                std::to_string(err.value("code", 0)) + "]: " +
                err.value("message", ""));
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(no code actions available)");
        }

        auto& result = response["result"];

        // Result is an array of CodeAction (or Command)
        if (!result.is_array() || result.empty()) {
            return std::string("(no code actions available)");
        }

        std::string output = std::to_string(result.size()) + " code actions:\n";
        for (size_t i = 0; i < result.size(); i++) {
            const auto& action = result[i];
            std::string title = action.value("title", "(unnamed)");
            std::string kind = action.value("kind", "");

            output += std::to_string(i + 1) + ". \"" + title + "\"";
            if (!kind.empty()) {
                // Extract short kind name after the last '/'
                auto slash = kind.rfind('/');
                if (slash != std::string::npos) {
                    output += " (" + kind.substr(slash + 1) + ")";
                } else {
                    output += " (" + kind + ")";
                }
            }
            output += "\n";
        }

        return output;
    };

    return t;
}
