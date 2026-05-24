#include "mcp/mcp_registry.h"

#include <algorithm>

// ===================================================================
// Construction / Destruction
// ===================================================================

McpRegistry::~McpRegistry() {
    // Shut down all running servers.
    for (auto& [name, server] : servers_) {
        if (server.running && server.client) {
            server.client->shutdown();
        }
    }
}

// ===================================================================
// Lifecycle
// ===================================================================

Result<void> McpRegistry::start_server(const McpEndpoint& config) {
    // Check for duplicate server names.
    if (servers_.count(config.name)) {
        if (servers_[config.name].running) {
            return std::unexpected(
                std::string("MCP server already running: ") + config.name);
        }
        // Server exists but not running — remove it so we can recreate.
        servers_.erase(config.name);
    }

    auto client = std::make_unique<McpClient>();
    Result<void> start_result;

    if (config.transport == "stdio" || config.transport.empty()) {
        // Stdio transport.
        start_result = client->start_stdio(
            config.command, config.args, config.cwd, config.env, config.timeout_sec);
    } else if (config.transport == "streamable-http") {
        // HTTP transport.
        start_result = client->start_http(
            config.url, config.api_key, config.timeout_sec);
    } else {
        return std::unexpected(
            std::string("Unknown MCP transport: ") + config.transport);
    }

    if (!start_result) {
        return std::unexpected(
            std::string("Failed to start MCP server '") + config.name +
            "': " + start_result.error());
    }

    // Discover tools.
    auto tools_result = client->list_tools();
    if (!tools_result) {
        client->shutdown();
        return std::unexpected(
            std::string("Failed to list tools from MCP server '") + config.name +
            "': " + tools_result.error());
    }

    // Namespace the tools.
    std::vector<Tool> namespaced_tools;
    for (auto& tool : *tools_result) {
        tool.name = tool_namespace(config.name, tool.name);
        namespaced_tools.push_back(std::move(tool));
    }

    McpServer server;
    server.config = config;
    server.client = std::move(client);
    server.tools = std::move(namespaced_tools);
    server.running = true;
    servers_[config.name] = std::move(server);

    return {};
}

Result<void> McpRegistry::stop_server(const std::string& name) {
    auto it = servers_.find(name);
    if (it == servers_.end() || !it->second.running) {
        // Not an error according to the plan: "Stop unknown server — no-op, no crash"
        return {};
    }

    if (it->second.client) {
        it->second.client->shutdown();
    }
    it->second.running = false;
    it->second.tools.clear();

    return {};
}

bool McpRegistry::is_running(const std::string& name) const {
    auto it = servers_.find(name);
    if (it == servers_.end())
        return false;
    // Check both the registry flag AND the actual client connection.
    // A server may be marked running in the registry but the underlying
    // McpClient may have detected a broken pipe (e.g. process crashed).
    if (!it->second.running)
        return false;
    if (it->second.client && !it->second.client->is_running())
        return false;
    return true;
}

bool McpRegistry::has_running_servers() const {
    for (const auto& [name, server] : servers_) {
        if (server.running)
            return true;
    }
    return false;
}

std::set<std::string> McpRegistry::running_server_names() const {
    std::set<std::string> names;
    for (const auto& [name, server] : servers_) {
        if (server.running)
            names.insert(name);
    }
    return names;
}

std::vector<McpEndpoint> McpRegistry::running_servers() const {
    std::vector<McpEndpoint> result;
    for (const auto& [name, server] : servers_) {
        if (server.running)
            result.push_back(server.config);
    }
    return result;
}

// ===================================================================
// Tool management
// ===================================================================

std::vector<Tool> McpRegistry::all_tools() const {
    std::vector<Tool> all;
    for (const auto& [name, server] : servers_) {
        if (!server.running)
            continue;
        all.insert(all.end(), server.tools.begin(), server.tools.end());
    }
    return all;
}

Result<std::string> McpRegistry::execute_tool(const std::string& namespaced_name,
                                               const json& args) {
    std::string server_name, tool_name;
    if (!parse_namespaced(namespaced_name, server_name, tool_name)) {
        return std::unexpected(
            std::string("Invalid namespaced tool name: ") + namespaced_name +
            " (expected format: mcp_<server>_<tool>)");
    }

    auto it = servers_.find(server_name);
    if (it == servers_.end()) {
        return std::unexpected(
            std::string("MCP server not found: ") + server_name);
    }

    if (!it->second.running) {
        return std::unexpected(
            std::string("MCP server is not running: ") + server_name);
    }

    if (!it->second.client) {
        return std::unexpected(
            std::string("MCP client not initialized for server: ") + server_name);
    }

    return it->second.client->call_tool(tool_name, args);
}

Result<std::vector<Tool>> McpRegistry::refresh_tools() {
    std::vector<Tool> all;

    for (auto& [name, server] : servers_) {
        if (!server.running || !server.client)
            continue;

        auto tools_result = server.client->list_tools();
        if (!tools_result) {
            // Server may have crashed — mark it as not running.
            server.running = false;
            server.tools.clear();
            continue;
        }

        // Namespace the tools.
        std::vector<Tool> namespaced_tools;
        for (auto& tool : *tools_result) {
            tool.name = tool_namespace(name, tool.name);
            namespaced_tools.push_back(std::move(tool));
        }

        server.tools = namespaced_tools;
        all.insert(all.end(), namespaced_tools.begin(), namespaced_tools.end());
    }

    return all;
}
