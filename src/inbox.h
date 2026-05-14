#pragma once

#include "config.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// A message in an agent's inbox.
struct InboxMessage {
    std::string from;    // sender agent name
    std::string message; // markdown body
};

/// Per-agent FIFO message queues.
/// Thread-safe: all public methods lock internally.
class Inbox {
  public:
    Inbox() = default;

    /// Register an agent by name. Returns false if name already taken.
    bool register_agent(const std::string& name);

    /// Unregister an agent. All undelivered messages are discarded.
    void unregister_agent(const std::string& name);

    /// Send a message to a recipient.
    /// Returns "delivered" or "no such recipient".
    Result<std::string> send_message(const std::string& from,
        const std::string& to,
        const std::string& message);

    /// Dequeue the next message for the named agent.
    /// Returns std::nullopt if the queue is empty.
    std::optional<InboxMessage> next_message(const std::string& agent_name);

    /// Return all registered agent names.
    std::vector<std::string> all_agent_names() const;

    /// Check if an agent name is registered.
    bool is_registered(const std::string& name) const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<InboxMessage>> queues_;
    std::unordered_set<std::string> agents_;
};
