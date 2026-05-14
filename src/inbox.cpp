#include "inbox.h"

#include <algorithm>

// ── Inbox ────────────────────────────────────────────────────────────

bool Inbox::register_agent(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto [it, inserted] = agents_.insert(name);
    if (inserted) {
        queues_[name] = {}; // ensure queue exists
    }
    return inserted;
}

void Inbox::unregister_agent(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    agents_.erase(name);
    queues_.erase(name);
}

Result<std::string> Inbox::send_message(const std::string& from,
    const std::string& to,
    const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (agents_.find(to) == agents_.end()) {
        return std::unexpected("no such recipient");
    }

    queues_[to].push_back(InboxMessage{from, message});
    return std::string("delivered");
}

std::optional<InboxMessage> Inbox::next_message(const std::string& agent_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = queues_.find(agent_name);
    if (it == queues_.end() || it->second.empty()) {
        return std::nullopt;
    }

    auto msg = std::move(it->second.front());
    it->second.erase(it->second.begin());
    return msg;
}

std::vector<std::string> Inbox::all_agent_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(agents_.size());
    for (const auto& name : agents_) {
        names.push_back(name);
    }
    return names;
}

bool Inbox::is_registered(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return agents_.find(name) != agents_.end();
}
