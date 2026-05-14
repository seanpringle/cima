#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct GroupMessage {
    int id = 0;
    std::string from;      // sender name (agent name or "user")
    std::string summary;   // short markdown
    std::string body;      // long detailed markdown
    std::vector<std::string> tags;  // @mentioned entities
};

/// Pending notification for an agent who was @mentioned.
struct PendingNotification {
    std::string from;     // who sent the message
    std::string summary;  // what was said (short summary)
    std::string body;     // the full message body
};

/// Shared group channel across all sessions.
/// Thread-safe: all public methods lock internally.
class GroupChannel {
  public:
    GroupChannel() = default;

    // ── Message API ──

    /// Post a message to the channel. Scans summary+body for @mentions
    /// and records tags automatically. Returns the new message id.
    int post_message(const std::string& from,
        const std::string& summary,
        const std::string& body);

    /// Return all messages (truncated to last 200).
    std::vector<GroupMessage> list_all_messages() const;

    /// Return messages with id > since (ordered by id).
    std::vector<GroupMessage> list_messages_since(int since) const;

    /// Read the full body of a specific message.
    std::optional<GroupMessage> read_message_body(int id) const;

    // ── Agent lifecycle ──

    /// Register an agent (a session) with this channel.
    /// @param tab_id  unique tab/session id
    /// @param name    the agent's Culture ship name
    void register_agent(int tab_id, const std::string& name);

    /// Unregister an agent when its tab closes.
    void unregister_agent(int tab_id);

    /// Mark an agent as busy (true) or idle (false).
    void set_agent_busy(int tab_id, bool busy);

    /// Check whether a name matches any registered agent (exact or prefix).
    /// Returns the matched agent name, or empty string.
    std::string match_agent_name(const std::string& fragment) const;

    /// Return all registered agent names.
    std::vector<std::string> all_agent_names() const;

    /// Return all registered (tab_id, name) pairs.
    std::vector<std::pair<int, std::string>> all_agents() const;

    // ── Pending notifications ──

    /// Check if the named agent has any pending notifications.
    /// Returns and clears them.
    std::vector<PendingNotification> consume_notifications(const std::string& agent_name);

  private:
    mutable std::mutex mutex_;
    std::vector<GroupMessage> messages_;
    int next_id_ = 1;

    struct AgentInfo {
        int tab_id = 0;
        std::string name;
        bool busy = false;
        std::vector<PendingNotification> notifications;
    };
    std::vector<AgentInfo> agents_;

    // Scan text for @mentions and return matched tags.
    std::vector<std::string> scan_tags(const std::string& summary,
        const std::string& body,
        const std::string& from) const;
};
