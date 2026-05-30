#pragma once

#include "config.h"         // for McpEndpoint
#include "mcp/mcp_client.h" // for McpClient, Tool

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// McpRegistry — manages multiple McpClient instances
//
// Provides tool discovery aggregation across all running MCP servers and
// routes tool execution to the correct server.
//
// Tools are namespaced as "mcp_<servername>_<toolname>" to avoid collisions.
// ---------------------------------------------------------------------------

class McpRegistry {
  public:
    McpRegistry() = default;
    ~McpRegistry(); // shuts down all running servers

    McpRegistry(const McpRegistry&) = delete;
    McpRegistry& operator=(const McpRegistry&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────

    /// Start an MCP server from its endpoint configuration.
    /// Performs initialize handshake + tools/list, registers tools.
    /// Returns an error if the server fails to start.
    Result<void> start_server(const McpEndpoint& config);

    /// Stop a running MCP server by name.  Removes its tools from the registry.
    /// Returns an error if the server is not running.
    Result<void> stop_server(const std::string& name);

    /// True if the named server is currently running.
    bool is_running(const std::string& name) const;

    /// True if at least one server is running.
    bool has_running_servers() const;

    /// Set of currently running server names.
    std::set<std::string> running_server_names() const;

    /// Get the endpoint configs of all currently running servers.
    std::vector<McpEndpoint> running_servers() const;

    // ── Tool management ──────────────────────────────────────────────

    /// Return all tools from all running servers, namespaced as
    /// "mcp_<server>_<tool>".
    std::vector<Tool> all_tools() const;

    /// Execute a namespaced tool.  The name must be in the form
    /// "mcp_<servername>_<toolname>".
    Result<std::string> execute_tool(const std::string& namespaced_name, const json& args);

    /// Re-discover tools from all running servers (re-issue tools/list).
    /// Returns the union of all tools (namespaced).
    Result<std::vector<Tool>> refresh_tools();

  private:
    struct McpServer {
        McpEndpoint config;
        std::unique_ptr<McpClient> client;
        std::vector<Tool> tools; // currently-discovered tools (namespaced)
        std::map<std::string, std::string> tool_original_names; // namespaced name -> original MCP tool name
        bool running = false;
    };

    /// Replace characters not allowed in OpenAI tool names with safe ones.
    /// Keeps [a-zA-Z0-9_-], replaces '.' and ' ', and also replaces '_'
    /// with '-' to avoid ambiguity with the '_' namespace separator.
    static std::string sanitize_name(const std::string& name) {
        std::string s = name;
        for (auto& c : s) {
            if (c == '.' || c == ' ' || c == '_')
                c = '-';
        }
        return s;
    }

    /// Build the namespaced name for a tool.
    /// Uses underscores so the result matches OpenAI's required pattern
    /// ^[a-zA-Z0-9_-]+$.
    static std::string tool_namespace(const std::string& server_name, const std::string& tool_name) {
        return "mcp_" + sanitize_name(server_name) + "_" + sanitize_name(tool_name);
    }

    /// Parse a namespaced name back into server and tool parts.
    /// Assumes both server and tool names have been sanitize_name()'d so
    /// they contain no underscores — the first '_' after the "mcp_" prefix
    /// is the unambiguous separator.
    /// Returns false if the name is not in the expected format.
    static bool parse_namespaced(const std::string& namespaced, std::string& server_name, std::string& tool_name) {
        // Format: "mcp_<server>_<tool>"
        if (namespaced.rfind("mcp_", 0) != 0)
            return false;
        auto rest = namespaced.substr(4); // skip "mcp_"
        auto sep = rest.find('_');
        if (sep == std::string::npos || sep == 0 || sep == rest.size() - 1)
            return false;
        server_name = rest.substr(0, sep);
        tool_name = rest.substr(sep + 1);
        return true;
    }

    std::map<std::string, McpServer> servers_;
};
