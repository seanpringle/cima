#include "group_channel.h"

#include <algorithm>
#include <cctype>
#include <sstream>

GroupMessage make_msg(int id,
    const std::string& from,
    std::string message,
    std::vector<std::string> tags) {
    return GroupMessage{id, from, std::move(message), std::move(tags)};
}

// ── Helpers ──────────────────────────────────────────────────────────

/// Check if character is valid in an agent name (alphanumeric or hyphen).
static bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
}

/// Find @mentions in text. Returns the matched text after @ (including hyphens).
static std::vector<std::string> find_mentions(const std::string& text) {
    std::vector<std::string> mentions;
    size_t pos = 0;
    while (true) {
        pos = text.find('@', pos);
        if (pos == std::string::npos)
            break;
        ++pos; // skip @
        if (pos >= text.size())
            break;
        // Read the mention (stop at non-name char)
        size_t start = pos;
        while (pos < text.size() && is_name_char(text[pos]))
            ++pos;
        std::string mention = text.substr(start, pos - start);
        if (!mention.empty())
            mentions.push_back(mention);
    }
    return mentions;
}

// ── GroupChannel ─────────────────────────────────────────────────────

int GroupChannel::post_message(const std::string& from,
    const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto tags = scan_tags(message, from);

    int id = next_id_++;

    // Prune old messages (keep last 500)
    while (messages_.size() >= 500)
        messages_.erase(messages_.begin());

    // Handle @everyone / @all
    bool everyone = false;
    bool all_tag = false;
    for (const auto& t : tags) {
        if (t == "everyone") {
            everyone = true;
            break;
        }
        if (t == "all") {
            all_tag = true;
        }
    }

    // Determine which agents to notify
    // Collect unique agent names that should receive notifications
    std::vector<std::string> notify_names;

    if (everyone || all_tag) {
        everyone = true; // unify the flag
        // Notify all agents except the sender
        for (const auto& agent : agents_) {
            if (agent.name != from) {
                notify_names.push_back(agent.name);
            }
        }
    } else {
        for (const auto& tag : tags) {
            if (tag == "user" || tag == "everyone" || tag == "all")
                continue;

            // Find matching agent
            for (const auto& agent : agents_) {
                if (agent.name == from)
                    continue; // skip sender

                // Normalize: lowercase compare
                std::string tag_lower = tag;
                std::string name_lower = agent.name;
                for (auto& c : tag_lower) c = std::tolower(static_cast<unsigned char>(c));
                for (auto& c : name_lower) c = std::tolower(static_cast<unsigned char>(c));

                bool matched = (tag_lower == name_lower);
                // Prefix match (e.g. "@Very" matches "Very-Little-Gravitas-Indeed")
                if (!matched && name_lower.find(tag_lower) != std::string::npos) {
                    if (name_lower.find(tag_lower) == 0 ||
                        name_lower.find("-" + tag_lower) != std::string::npos) {
                        matched = true;
                    }
                }

                if (matched) {
                    if (std::find(notify_names.begin(), notify_names.end(), agent.name) == notify_names.end()) {
                        notify_names.push_back(agent.name);
                    }
                }
            }
        }
    }

    // If @everyone/@all, ensure all agent names are in the tags (for display)
    if (everyone) {
        for (const auto& agent : agents_) {
            if (agent.name != from &&
                std::find(tags.begin(), tags.end(), agent.name) == tags.end()) {
                tags.push_back(agent.name);
            }
        }
    }

    // Now store the message with expanded tags
    messages_.push_back(make_msg(id, from, message, tags));

    // Deliver notifications to all matched agents (include sender info)
    for (const auto& name : notify_names) {
        for (auto& agent : agents_) {
            if (agent.name == name) {
                PendingNotification note;
                note.from = from;
                note.message = message;
                agent.notifications.push_back(std::move(note));
                break;
            }
        }
    }

    return id;
}

std::vector<GroupMessage> GroupChannel::list_all_messages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (messages_.size() > 200) {
        return std::vector<GroupMessage>(messages_.end() - 200, messages_.end());
    }
    return messages_;
}

std::vector<GroupMessage> GroupChannel::list_messages_since(int since) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GroupMessage> result;
    for (const auto& m : messages_) {
        if (m.id > since)
            result.push_back(m);
    }
    return result;
}

std::vector<GroupMessage> GroupChannel::read_new_messages(const std::string& agent_name) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Get the current cursor (defaults to 0 on first call)
    int last_id = last_read_[agent_name];

    std::vector<GroupMessage> result;
    for (const auto& m : messages_) {
        if (m.id > last_id)
            result.push_back(m);
    }

    // Advance cursor to the latest message id
    if (!messages_.empty()) {
        last_read_[agent_name] = messages_.back().id;
    }

    return result;
}

void GroupChannel::register_agent(int tab_id, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Remove if already registered (shouldn't happen with unique tab_ids)
    agents_.erase(
        std::remove_if(agents_.begin(), agents_.end(),
            [tab_id](const AgentInfo& a) { return a.tab_id == tab_id; }),
        agents_.end());
    agents_.push_back({tab_id, name, false, {}});

    // Initialize cursor at 0 (will return all existing messages on first read)
    last_read_[name];  // default-constructs to 0
}

void GroupChannel::unregister_agent(int tab_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    agents_.erase(
        std::remove_if(agents_.begin(), agents_.end(),
            [tab_id](const AgentInfo& a) { return a.tab_id == tab_id; }),
        agents_.end());
}

void GroupChannel::set_agent_busy(int tab_id, bool busy) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& a : agents_) {
        if (a.tab_id == tab_id) {
            a.busy = busy;
            return;
        }
    }
}

std::string GroupChannel::match_agent_name(const std::string& fragment) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string frag_lower = fragment;
    for (auto& c : frag_lower) c = std::tolower(static_cast<unsigned char>(c));

    for (const auto& a : agents_) {
        std::string name_lower = a.name;
        for (auto& c : name_lower) c = std::tolower(static_cast<unsigned char>(c));
        if (name_lower.find(frag_lower) != std::string::npos)
            return a.name;
    }
    return {};
}

std::vector<std::string> GroupChannel::all_agent_names() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> names;
    names.reserve(agents_.size());
    for (const auto& a : agents_)
        names.push_back(a.name);
    return names;
}

std::vector<std::pair<int, std::string>> GroupChannel::all_agents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::pair<int, std::string>> result;
    result.reserve(agents_.size());
    for (const auto& a : agents_)
        result.emplace_back(a.tab_id, a.name);
    return result;
}

std::vector<PendingNotification> GroupChannel::consume_notifications(const std::string& agent_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& a : agents_) {
        if (a.name == agent_name) {
            auto notes = std::move(a.notifications);
            a.notifications.clear();
            return notes;
        }
    }
    return {};
}

std::vector<std::string> GroupChannel::scan_tags(const std::string& body,
    const std::string& from) const {
    std::vector<std::string> tags;

    auto add_tags = [&](const std::string& text) {
        auto mentions = find_mentions(text);
        for (auto& m : mentions) {
            if (std::find(tags.begin(), tags.end(), m) == tags.end()) {
                tags.push_back(m);
            }
        }
    };

    add_tags(body);

    // Also scan for agent names that appear without @ prefix
    // (but don't auto-tag the sender)
    std::string combined = body;
    std::vector<std::string> agent_names;
    for (const auto& a : agents_) {
        if (a.name != from)
            agent_names.push_back(a.name);
    }

    for (const auto& name : agent_names) {
        if (name.empty()) continue;
        size_t pos = 0;
        while ((pos = combined.find(name, pos)) != std::string::npos) {
            // Check it's a whole-word match (preceded by non-name-char or start)
            bool preceded = (pos == 0) || !is_name_char(combined[pos - 1]);
            bool followed = (pos + name.size() >= combined.size()) ||
                !is_name_char(combined[pos + name.size()]);
            if (preceded && followed) {
                if (std::find(tags.begin(), tags.end(), name) == tags.end()) {
                    tags.push_back(name);
                }
                break; // one match per name per message
            }
            pos += name.size();
        }
    }

    return tags;
}
