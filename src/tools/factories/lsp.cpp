#include "tools.h"
#include "lsp/json_rpc.h"
#include "lsp/lsp_client.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ===================================================================
// get_lsp_diagnostics
// ===================================================================

Tool make_get_lsp_diagnostics_tool(LspClient** lsp_ptr) {
    Tool t;
    t.name = "get_lsp_diagnostics";
    t.description =
        "Get compiler errors and warnings for a file using the LSP "
        "(clangd) language server.\n"
        "The file must already exist on disk. Returns diagnostics "
        "with severity, message, file, line, and column.\n"
        "LSP must be started from the application UI to use this tool.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 15;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file to check"}}}}},
        {"required", {"path"}}};
    t.execute = [lsp_ptr](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp || !lsp->is_running()) {
            return std::unexpected("LSP server is not running.");
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
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                std::string("Failed to sync file with LSP server: ") +
                sync.error());
        }

        // Request diagnostics (LSP 3.17 pull diagnostics)
        auto resp = lsp->request("textDocument/diagnostic",
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

        // LSP 3.17 pull diagnostics returns a DocumentDiagnosticReport.
        // Check the kind first — only "full" reports have items.
        std::string kind = result.value("kind", std::string());
        bool is_unchanged = (kind == "unchanged");

        // Build formatted output
        std::string output;
        int total_count = 0;

        // Helper lambda to format a single Diagnostic object
        auto format_diagnostic = [&](const json& d, const std::string& display_path) {
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

            // Include code if present
            if (d.contains("code") && !d["code"].is_null()) {
                if (d["code"].is_string()) {
                    output += "  code: " + d["code"].get<std::string>() + "\n";
                } else if (d["code"].is_number()) {
                    output += "  code: " + std::to_string(d["code"].get<int>()) + "\n";
                }
            }
        };

        // Diagnostics for the primary document (items inside the report)
        if (!is_unchanged && result.contains("items") && result["items"].is_array()) {
            auto& items = result["items"];
            for (const auto& d : items) {
                format_diagnostic(d, raw_path);
            }
        }

        // Diagnostics from related documents (cross-file issues)
        // Each value in relatedDocuments is itself a DocumentDiagnosticReport
        // with kind and items.
        if (result.contains("relatedDocuments") &&
            result["relatedDocuments"].is_object()) {
            for (auto it = result["relatedDocuments"].begin();
                 it != result["relatedDocuments"].end(); ++it) {
                const auto& related_uri = it.key();
                auto& related_report = it.value();

                // Only process "full" reports
                if (related_report.value("kind", std::string()) != "full")
                    continue;
                if (!related_report.contains("items") || !related_report["items"].is_array())
                    continue;

                // Convert URI to path for display
                auto related_path = lsp::uri_to_path(related_uri);
                const std::string& display_path =
                    related_path ? *related_path : related_uri;

                for (const auto& d : related_report["items"]) {
                    format_diagnostic(d, display_path);
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

Tool make_get_lsp_hover_tool(LspClient** lsp_ptr) {
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
    t.execute = [lsp_ptr](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
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
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request hover
        auto resp = lsp->request("textDocument/hover", {
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

Tool make_get_lsp_definition_tool(LspClient** lsp_ptr) {
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
    t.execute = [lsp_ptr](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
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
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request definition
        auto resp = lsp->request("textDocument/definition", {
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

Tool make_get_lsp_completion_tool(LspClient** lsp_ptr) {
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
    t.execute = [lsp_ptr](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
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
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request completion
        auto resp = lsp->request("textDocument/completion", {
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

Tool make_get_lsp_code_actions_tool(LspClient** lsp_ptr) {
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
    t.execute = [lsp_ptr](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
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
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // First, get diagnostics to provide context for code actions
        auto diag_resp = lsp->request("textDocument/pullDiagnostics", {
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
        auto resp = lsp->request("textDocument/codeAction", {
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

// ===================================================================
// Helpers for applying LSP edits to files
// ===================================================================

/// Convert 0-based line and column (UTF-8 byte offset) to a byte offset
/// within the given content string.
/// This is a simple newline-counting approach.  For ASCII content it is
/// exact; for non-ASCII UTF-8 it assumes the column is given in bytes
/// (which holds when the client advertises "offsetEncoding": "utf-8").
/// TODO: support UTF-16 offset encoding when not advertising utf-8.
static size_t position_to_offset(const std::string& content, int line, int col) {
    size_t offset = 0;
    int current_line = 0;
    while (current_line < line && offset < content.size()) {
        auto nl = content.find('\n', offset);
        if (nl == std::string::npos)
            return content.size();
        offset = nl + 1;
        current_line++;
    }
    if (current_line != line)
        return content.size();

    size_t pos = offset + static_cast<size_t>(col);
    if (pos > content.size())
        pos = content.size();
    return pos;
}

/// Apply an array of TextEdit (from LSP) to the content string in-place.
/// Edits are sorted by range start position in descending order so that
/// applying them does not invalidate positions of subsequent edits.
static void apply_text_edits_to_content(std::string& content, const json& edits) {
    // Build a vector of (start_offset, end_offset, newText) tuples
    struct EditRange {
        size_t start;
        size_t end;
        std::string new_text;
    };

    std::vector<EditRange> edit_list;
    for (const auto& edit : edits) {
        auto& range = edit["range"];
        auto& start_pos = range["start"];
        auto& end_pos = range["end"];

        size_t start_off = position_to_offset(
            content, start_pos["line"].get<int>(), start_pos["character"].get<int>());
        size_t end_off = position_to_offset(
            content, end_pos["line"].get<int>(), end_pos["character"].get<int>());

        edit_list.push_back({start_off, end_off, edit["newText"].get<std::string>()});
    }

    // Sort in descending order by start offset (so edits don't shift each other)
    std::sort(edit_list.begin(), edit_list.end(),
        [](const EditRange& a, const EditRange& b) {
            return a.start > b.start;
        });

    // Apply edits
    for (const auto& e : edit_list) {
        content.replace(e.start, e.end - e.start, e.new_text);
    }
}

/// Apply a single file's TextEdit[] to disk and sync with LSP.
static Result<void> apply_text_edits_to_file(LspClient& lsp,
                                              const std::string& uri,
                                              const json& edits) {
    auto path = lsp::uri_to_path(uri);
    if (!path) {
        return std::unexpected("Invalid URI: " + uri);
    }

    // Read the current file content
    std::ifstream infile(*path, std::ios::binary);
    if (!infile.is_open()) {
        return std::unexpected("Cannot open file for reading: " + *path);
    }
    std::string content((std::istreambuf_iterator<char>(infile)),
                         std::istreambuf_iterator<char>());
    infile.close();

    // Apply edits
    apply_text_edits_to_content(content, edits);

    // Write back
    std::ofstream outfile(*path, std::ios::binary);
    if (!outfile.is_open()) {
        return std::unexpected("Cannot open file for writing: " + *path);
    }
    outfile.write(content.data(), content.size());
    outfile.close();

    // Sync with LSP server
    auto lang = LspClient::language_id_from_extension(*path);
    auto sync = lsp.ensure_file_synced(uri, lang, content);
    if (!sync) {
        // Non-fatal: the file is written, just log
        std::cerr << "apply_text_edits_to_file: failed to sync " << *path
                  << " with LSP: " << sync.error() << std::endl;
    }

    return {};
}

/// Apply a WorkspaceEdit (the result of textDocument/rename) to disk.
/// Handles both `changes` (map of URI → TextEdit[]) and
/// `documentChanges` (array of TextDocumentEdit).
/// Returns a map of URI → number of edits applied for the report.
static Result<std::map<std::string, int>> apply_workspace_edit(
    LspClient& lsp, const json& workspace_edit) {
    std::map<std::string, int> per_file_counts;

    // Handle `changes` format
    if (workspace_edit.contains("changes") && workspace_edit["changes"].is_object()) {
        for (auto it = workspace_edit["changes"].begin();
             it != workspace_edit["changes"].end(); ++it) {
            const std::string& uri = it.key();
            const json& edits = it.value();
            if (!edits.is_array() || edits.empty())
                continue;

            auto result = apply_text_edits_to_file(lsp, uri, edits);
            if (!result) {
                return std::unexpected(result.error());
            }
            per_file_counts[uri] = static_cast<int>(edits.size());
        }
    }

    // Handle `documentChanges` format
    if (workspace_edit.contains("documentChanges") &&
        workspace_edit["documentChanges"].is_array()) {
        for (const auto& doc_change : workspace_edit["documentChanges"]) {
            const std::string& uri = doc_change["textDocument"]["uri"];
            const json& edits = doc_change["edits"];
            if (!edits.is_array() || edits.empty())
                continue;

            auto result = apply_text_edits_to_file(lsp, uri, edits);
            if (!result) {
                return std::unexpected(result.error());
            }
            per_file_counts[uri] += static_cast<int>(edits.size());
        }
    }

    return per_file_counts;
}

// ===================================================================
// get_lsp_rename
// ===================================================================

Tool make_get_lsp_rename_tool(LspClient** lsp_ptr) {
    Tool t;
    t.name = "get_lsp_rename";
    t.description =
        "Rename a symbol across the entire project.\n"
        "Specify the file path, line, and character of the symbol to rename, "
        "and the new name.  Uses clangd's rename capability (textDocument/rename).\n"
        "This tool modifies files on disk — use with care.\n"
        "Start clangd from the Config tab (LSP / clangd: Start LSP button) to use this tool.";
    t.permission = ToolPermission::Write;
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file containing the symbol"}}},
             {"line",
                {{"type", "integer"},
                    {"description", "0-based line number of the symbol"}}},
             {"character",
                {{"type", "integer"},
                    {"description", "0-based column offset of the symbol"}}},
             {"new_name",
                {{"type", "string"},
                    {"description", "The new name for the symbol"}}}}},
        {"required", {"path", "line", "character", "new_name"}}};
    int timeout = t.timeout_sec;
    t.execute = [lsp_ptr, timeout](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
        }

        auto raw_path = args.value("path", std::string());
        int line = args.value("line", -1);
        int character = args.value("character", -1);
        auto new_name = args.value("new_name", std::string());

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0) {
            return std::unexpected(std::string("line must be >= 0"));
        }
        if (character < 0) {
            return std::unexpected(std::string("character must be >= 0"));
        }
        if (new_name.empty()) {
            return std::unexpected(std::string("new_name is required"));
        }

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(static_cast<size_t>(size), '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // First, check if the symbol is renameable via prepareRename
        auto prepare = lsp->request("textDocument/prepareRename", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}}
        }, timeout);

        if (!prepare) {
            // Method not found → clangd too old
            if (prepare.error().find("MethodNotFound") != std::string::npos) {
                return std::unexpected(
                    std::string("clangd version too old — rename requires clangd >= 6.0.0. "
                                "Please upgrade your clangd installation."));
            }
            return std::unexpected(prepare.error());
        }

        // Check for errors or null result (null = not renameable)
        if (prepare->contains("error")) {
            auto& err = (*prepare)["error"];
            int code = err.value("code", 0);
            std::string msg = err.value("message", "");
            if (code == lsp::ErrorCodes::MethodNotFound) {
                return std::unexpected(
                    std::string("LSP method not found (clangd too old?): ") + msg);
            }
            return std::unexpected(
                std::string("LSP error [") + std::to_string(code) +
                "]: " + msg);
        }

        if (!prepare->contains("result") || (*prepare)["result"].is_null()) {
            return std::string("(symbol is not renameable at this location)");
        }

        // Symbol is renameable — execute the rename
        auto resp = lsp->request("textDocument/rename", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}},
            {"newName", new_name}
        }, timeout);

        if (!resp) {
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            return std::unexpected(
                std::string("LSP error [") +
                std::to_string(err.value("code", 0)) +
                "]: " + err.value("message", ""));
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(rename produced no changes)");
        }

        auto& workspace_edit = response["result"];

        // Apply the workspace edit to disk
        auto apply_result = apply_workspace_edit(*lsp, workspace_edit);
        if (!apply_result) {
            return std::unexpected(
                std::string("Failed to apply rename edits: ") +
                apply_result.error());
        }

        // Build the report
        std::string output = "Renamed → '" + new_name + "':\n";
        int total_files = 0;
        int total_edits = 0;
        for (const auto& [uri, count] : *apply_result) {
            auto path = lsp::uri_to_path(uri);
            const std::string& display = path ? *path : uri;
            output += "- " + display + ": " + std::to_string(count) +
                      (count == 1 ? " occurrence" : " occurrences") + " updated\n";
            total_files++;
            total_edits += count;
        }
        // Prepend summary
        std::string summary = std::to_string(total_edits) +
            (total_edits == 1 ? " change" : " changes") +
            " across " + std::to_string(total_files) +
            (total_files == 1 ? " file" : " files") + ":\n";
        output = summary + output;

        return output;
    };
    return t;
}

// ===================================================================
// get_lsp_format
// ===================================================================

Tool make_get_lsp_format_tool(LspClient** lsp_ptr) {
    Tool t;
    t.name = "get_lsp_format";
    t.description =
        "Format code in a file using clang-format (embedded in clangd).\n"
        "If start_line and end_line are provided, only that range is formatted. "
        "Otherwise the entire file is formatted.\n"
        "This tool modifies the file on disk — use with care.\n"
        "Start clangd from the Config tab (LSP / clangd: Start LSP button) to use this tool.";
    t.permission = ToolPermission::Write;
    t.timeout_sec = 15;
    int timeout = t.timeout_sec;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file to format"}}},
             {"start_line",
                {{"type", "integer"},
                    {"description",
                        "Optional: 0-based start line for range formatting"}}},
             {"end_line",
                {{"type", "integer"},
                    {"description",
                        "Optional: 0-based end line for range formatting (exclusive)"}}}}},
        {"required", {"path"}}};
    t.execute = [lsp_ptr, timeout](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
        }

        auto raw_path = args.value("path", std::string());
        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }

        // Optional range parameters
        bool has_start = args.contains("start_line") && !args["start_line"].is_null();
        bool has_end = args.contains("end_line") && !args["end_line"].is_null();

        int start_line = args.value("start_line", -1);
        int end_line = args.value("end_line", -1);

        if (has_start && start_line < 0) {
            return std::unexpected(std::string("start_line must be >= 0"));
        }
        if (has_end && end_line < 0) {
            return std::unexpected(std::string("end_line must be >= 0"));
        }
        if (has_start && has_end && end_line <= start_line) {
            return std::unexpected(
                std::string("end_line must be > start_line"));
        }

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(static_cast<size_t>(size), '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Determine which formatting method to call
        std::string method;
        json params;
        if (has_start && has_end) {
            method = "textDocument/rangeFormatting";
            params = {
                {"textDocument", {{"uri", uri}}},
                {"range", {
                    {"start", {{"line", start_line}, {"character", 0}}},
                    {"end", {{"line", end_line}, {"character", 0}}}
                }},
                {"options", {
                    {"tabSize", 4},
                    {"insertSpaces", true}
                }}
            };
        } else {
            method = "textDocument/formatting";
            params = {
                {"textDocument", {{"uri", uri}}},
                {"options", {
                    {"tabSize", 4},
                    {"insertSpaces", true}
                }}
            };
        }

        auto resp = lsp->request(method, params, timeout);
        if (!resp) {
            // Check if the error is MethodNotFound (older clangd without clang-format)
            if (resp.error().find("MethodNotFound") != std::string::npos) {
                return std::unexpected(
                    std::string("clangd version too old — formatting requires clangd >= 6.0.0. "
                                "Please upgrade your clangd installation."));
            }
            return std::unexpected(resp.error());
        }

        auto& response = *resp;
        if (response.contains("error")) {
            auto& err = response["error"];
            int code = err.value("code", 0);
            std::string msg = err.value("message", "");
            if (code == lsp::ErrorCodes::MethodNotFound) {
                return std::unexpected(
                    std::string("LSP method not found (clangd too old?): ") + msg);
            }
            return std::unexpected(
                std::string("LSP error [") + std::to_string(code) +
                "]: " + msg);
        }

        if (!response.contains("result") || response["result"].is_null()) {
            return std::string("(already formatted)");
        }

        auto& edits = response["result"];
        if (!edits.is_array() || edits.empty()) {
            return std::string("(already formatted)");
        }

        // Apply edits to the file
        auto apply = apply_text_edits_to_file(*lsp, uri, edits);
        if (!apply) {
            return std::unexpected(
                "Failed to apply formatting edits: " + apply.error());
        }

        int count = static_cast<int>(edits.size());
        return "Formatted " + raw_path + " (" + std::to_string(count) +
               (count == 1 ? " change" : " changes") + ")";
    };
    return t;
}

// ===================================================================
// Helpers for LSP response formatting
// ===================================================================

/// Convert an LSP SymbolKind number to a human-readable string.
static std::string symbol_kind_name(int kind) {
    switch (kind) {
        case 1:  return "file";
        case 2:  return "module";
        case 3:  return "namespace";
        case 4:  return "package";
        case 5:  return "class";
        case 6:  return "method";
        case 7:  return "property";
        case 8:  return "field";
        case 9:  return "constructor";
        case 10: return "enum";
        case 11: return "interface";
        case 12: return "function";
        case 13: return "variable";
        case 14: return "constant";
        case 15: return "string";
        case 16: return "number";
        case 17: return "boolean";
        case 18: return "array";
        case 19: return "object";
        case 20: return "key";
        case 21: return "null";
        case 22: return "enum member";
        case 23: return "struct";
        case 24: return "event";
        case 25: return "operator";
        case 26: return "type parameter";
        default: return "symbol";
    }
}

/// Recursively format a DocumentSymbol tree into output with indentation.
static void format_document_symbol(const json& symbol, std::string& output,
                                    int depth, int max_depth) {
    if (depth >= max_depth) return;

    // Indent
    for (int i = 0; i < depth; i++) {
        output += "  ";
    }
    output += "├── ";

    // Symbol name
    std::string name = symbol.value("name", "(unnamed)");
    int kind = symbol.value("kind", 0);
    output += symbol_kind_name(kind) + " " + name;

    // Detail (e.g. function signature)
    if (symbol.contains("detail") && symbol["detail"].is_string() &&
        !symbol["detail"].get<std::string>().empty()) {
        output += " : " + symbol["detail"].get<std::string>();
    }

    // Location
    if (symbol.contains("range") && symbol["range"].contains("start")) {
        auto& start = symbol["range"]["start"];
        int line = start.value("line", 0);
        int col = start.value("character", 0);
        output += " [" + std::to_string(line + 1) + ":" +
                  std::to_string(col + 1) + "]";
    }

    output += "\n";

    // Children
    if (symbol.contains("children") && symbol["children"].is_array()) {
        for (const auto& child : symbol["children"]) {
            format_document_symbol(child, output, depth + 1, max_depth);
        }
    }
}

// ===================================================================
// get_lsp_references
// ===================================================================

Tool make_get_lsp_references_tool(LspClient** lsp_ptr) {
    Tool t;
    t.name = "get_lsp_references";
    t.description =
        "Find all references to a symbol at a given file position.\n"
        "Returns locations grouped by file with line and column numbers.\n"
        "Uses the LSP (clangd) language server to query references.\n"
        "Start clangd from the Config tab (LSP / clangd: Start LSP button) to use this tool.";
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
                    {"description", "0-based column offset (in UTF-16 code units)"}}},
             {"include_declaration",
                {{"type", "boolean"},
                    {"description", "Include the declaration as a reference (default true)"}}},
             {"max_results",
                {{"type", "integer"},
                    {"description", "Maximum number of references to return (default 200)"}}}}},
        {"required", {"path", "line", "character"}}};
    int timeout = t.timeout_sec;
    t.execute = [lsp_ptr, timeout](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
        }

        auto raw_path = args.value("path", std::string());
        int line = args.value("line", -1);
        int character = args.value("character", -1);
        bool include_declaration = args.value("include_declaration", true);
        int max_results = args.value("max_results", 200);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (line < 0) {
            return std::unexpected(std::string("line must be >= 0"));
        }
        if (character < 0) {
            return std::unexpected(std::string("character must be >= 0"));
        }
        if (max_results < 1) max_results = 1;

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(static_cast<size_t>(size), '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request references
        auto resp = lsp->request("textDocument/references", {
            {"textDocument", {{"uri", uri}}},
            {"position", {{"line", line}, {"character", character}}},
            {"context", {{"includeDeclaration", include_declaration}}}
        }, timeout);

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
            return std::string("(no references found)");
        }

        auto& locations = response["result"];
        if (!locations.is_array() || locations.empty()) {
            return std::string("(no references found)");
        }

        // Group references by file
        std::map<std::string, std::vector<json>> grouped;
        for (const auto& loc : locations) {
            auto loc_uri = loc.value("uri", "");
            grouped[loc_uri].push_back(loc);
        }

        // Format output
        std::string output;
        int total = 0;
        for (const auto& [file_uri, refs] : grouped) {
            auto path = lsp::uri_to_path(file_uri);
            const std::string& display = path ? *path : file_uri;
            output += display + ":\n";

            int count = 0;
            for (const auto& ref : refs) {
                if (total >= max_results) {
                    output += "  ...(truncated, >" + std::to_string(max_results) +
                              " results — increase max_results)\n";
                    break;
                }
                auto& range = ref["range"];
                auto& start = range["start"];
                auto& end = range["end"];
                int sl = start.value("line", 0);
                int sc = start.value("character", 0);
                int el = end.value("line", 0);
                int ec = end.value("character", 0);

                output += "  [" + std::to_string(sl + 1) + ":" +
                          std::to_string(sc + 1) + " - " +
                          std::to_string(el + 1) + ":" +
                          std::to_string(ec + 1) + "]\n";
                count++;
                total++;
            }
            if (count == 0) continue; // shouldn't happen
        }

        if (total == 0) {
            return std::string("(no references found)");
        }

        // Prepend count
        std::string summary = std::to_string(total) +
            (total == 1 ? " reference" : " references") + " found:\n";
        output = summary + output;
        return output;
    };
    return t;
}

// ===================================================================
// get_lsp_document_symbols
// ===================================================================

Tool make_get_lsp_document_symbols_tool(LspClient** lsp_ptr) {
    Tool t;
    t.name = "get_lsp_document_symbols";
    t.description =
        "Get the symbol outline (structure) of a source file.\n"
        "Returns a tree of symbols (classes, functions, variables, namespaces, etc.) "
        "with their kinds and locations.\n"
        "Uses the LSP (clangd) language server to query document symbols.\n"
        "Start clangd from the Config tab (LSP / clangd: Start LSP button) to use this tool.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 10;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path",
                {{"type", "string"},
                    {"description", "Path to the source file"}}},
             {"max_depth",
                {{"type", "integer"},
                    {"description", "Maximum nesting depth for the symbol tree (default 5)"}}}}},
        {"required", {"path"}}};
    int timeout = t.timeout_sec;
    t.execute = [lsp_ptr, timeout](const json& args) -> Result<std::string> {
        auto* lsp = *lsp_ptr;
        if (!lsp || !lsp->is_running()) {
            return std::unexpected(
                std::string("LSP server is not running. "
                            "Click Start LSP in the Config tab to enable this tool."));
        }

        auto raw_path = args.value("path", std::string());
        int max_depth = args.value("max_depth", 5);

        if (raw_path.empty()) {
            return std::unexpected(std::string("path is required"));
        }
        if (max_depth < 1) max_depth = 1;
        if (max_depth > 20) max_depth = 20;

        std::string uri = lsp::path_to_uri(raw_path);

        // Read and sync file content
        std::ifstream file(raw_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return std::unexpected("Cannot open file: " + raw_path);
        }
        auto size = file.tellg();
        std::string content(static_cast<size_t>(size), '\0');
        file.seekg(0);
        file.read(content.data(), size);

        auto lang = LspClient::language_id_from_extension(raw_path);
        auto sync = lsp->ensure_file_synced(uri, lang, content);
        if (!sync) {
            return std::unexpected(
                "Failed to sync file with LSP server: " + sync.error());
        }

        // Request document symbols
        auto resp = lsp->request("textDocument/documentSymbol", {
            {"textDocument", {{"uri", uri}}}
        }, timeout);

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
            return std::string("(no symbols found)");
        }

        auto& symbols = response["result"];

        // Handle both DocumentSymbol[] (array of objects with children)
        // and SymbolInformation[] (flat array with location.uri)
        std::string output;
        int total = 0;

        if (symbols.is_array()) {
            for (const auto& sym : symbols) {
                if (sym.is_object()) {
                    // Check if it's a DocumentSymbol (has range) or
                    // SymbolInformation (has location)
                    if (sym.contains("range")) {
                        // DocumentSymbol (may or may not have children)
                        format_document_symbol(sym, output, 0, max_depth);
                        // Count this symbol
                        total++;
                        // Count children recursively
                        std::function<void(const json&)> count_children =
                            [&](const json& s) {
                                if (s.contains("children") && s["children"].is_array()) {
                                    for (const auto& c : s["children"]) {
                                        total++;
                                        count_children(c);
                                    }
                                }
                            };
                        count_children(sym);
                    } else if (sym.contains("location")) {
                        // SymbolInformation flat format
                        total++;
                        std::string name = sym.value("name", "(unnamed)");
                        int kind = sym.value("kind", 0);
                        auto& loc = sym["location"];
                        auto& range = loc["range"];
                        auto& start = range["start"];
                        int line = start.value("line", 0);
                        int col = start.value("character", 0);

                        std::string container = sym.value("containerName", "");
                        if (!container.empty()) {
                            output += container + "::";
                        }
                        output += symbol_kind_name(kind) + " " + name +
                                  " [" + std::to_string(line + 1) + ":" +
                                  std::to_string(col + 1) + "]\n";
                    }
                }
            }
        }

        if (total == 0) {
            return std::string("(no symbols found)");
        }

        std::string summary = std::to_string(total) +
            (total == 1 ? " symbol" : " symbols") + ":\n";
        output = summary + output;
        return output;
    };
    return t;
}
