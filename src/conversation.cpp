#include "conversation.h"

#include <fstream>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Add message helpers
// ---------------------------------------------------------------------------

int64_t Conversation::add_user(const std::string& content) {
    Message msg;
    msg.role = "user";
    msg.content = content;
    msg.suggested_retention = "preserve";
    int64_t id = next_id_++;
    messages_.push_back(std::move(msg));
    return id;
}

int64_t Conversation::add_notice(const std::string& content) {
    Message msg;
    msg.role = "user";
    msg.content = content;
    msg.suggested_retention = "droppable";
    int64_t id = next_id_++;
    messages_.push_back(std::move(msg));
    return id;
}

bool Conversation::add_skill(const std::string& name, const std::string& content) {
    if (!name.empty() && loaded_skill_names_.insert(name).second) {
        appended_system_ += "\n\n## Skill: " + name + "\n\n" + content;
        return true;
    }
    return false;
}

void Conversation::append_system(const std::string& content) {
    if (!content.empty()) {
        appended_system_ += "\n\n" + content;
    }
}

std::string Conversation::get_appended_system() const {
    return appended_system_;
}

int64_t Conversation::add_assistant(const std::string& content,
    const std::string& reasoning,
    const std::vector<ToolCall>& tool_calls) {
    Message msg;
    msg.role = "assistant";
    msg.reasoning_content = reasoning;
    msg.suggested_retention = "preserve";

    if (!tool_calls.empty()) {
        msg.content = std::nullopt;
        msg.tool_calls = tool_calls;
    } else {
        msg.content = content;
    }

    int64_t id = next_id_++;
    messages_.push_back(std::move(msg));
    return id;
}

void Conversation::add_tool(
    int64_t message_id, const std::string& tool_call_id, const std::string& content) {
    // Find the message by id (we use vector index + 1 as id)
    // message_id is 1-based, vector is 0-based
    size_t idx = static_cast<size_t>(message_id - 1);
    if (idx >= messages_.size())
        return;

    auto& msg = messages_[idx];
    for (auto& tc : msg.tool_calls) {
        if (tc.id == tool_call_id) {
            tc.result = sanitize_utf8(content);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// API payload builder
// ---------------------------------------------------------------------------

json Conversation::build_openai_payload(const std::string& system_prompt) const {
    json arr = json::array();

    arr.push_back({{"role", "system"}, {"content", sanitize_utf8(system_prompt)}});

    for (const auto& msg : messages_) {
        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            // Assistant with tool calls — expand into assistant + tool messages
            json j;
            j["role"] = "assistant";
            j["content"] = nullptr;
            if (!msg.reasoning_content.empty()) {
                j["reasoning_content"] = sanitize_utf8(msg.reasoning_content);
            }

            json tc_arr = json::array();
            for (const auto& tc : msg.tool_calls) {
                json tc_json;
                tc_json["id"] = tc.id;
                tc_json["type"] = "function";
                tc_json["function"] = {{"name", tc.name}, {"arguments", tc.arguments}};
                tc_arr.push_back(std::move(tc_json));
            }
            j["tool_calls"] = std::move(tc_arr);
            arr.push_back(std::move(j));

            // Emit tool result messages for every tool_call (even empty results).
            for (const auto& tc : msg.tool_calls) {
                json tr;
                tr["role"] = "tool";
                tr["tool_call_id"] = tc.id;
                tr["content"] = sanitize_utf8(tc.result);
                arr.push_back(std::move(tr));
            }
        } else if (msg.role == "tool") {
            json j;
            j["role"] = "tool";
            j["tool_call_id"] = msg.tool_call_id;
            j["content"] = sanitize_utf8(msg.content.value_or(""));
            arr.push_back(std::move(j));
        } else {
            // user, system, or assistant without tool calls
            json j;
            j["role"] = msg.role;
            j["content"] = sanitize_utf8(msg.content.value_or(""));

            if (msg.role == "assistant" && !msg.reasoning_content.empty()) {
                j["reasoning_content"] = sanitize_utf8(msg.reasoning_content);
            }

            arr.push_back(std::move(j));
        }
    }

    return arr;
}

// ---------------------------------------------------------------------------
// Token estimation
// ---------------------------------------------------------------------------

size_t Conversation::estimate_total_tokens() const {
    size_t total = 0;
    for (const auto& msg : messages_) {
        total += estimate_tokens(msg);
    }
    return total;
}

size_t Conversation::estimate_droppable_tokens() const {
    size_t total = 0;
    for (const auto& msg : messages_) {
        for (const auto& tc : msg.tool_calls) {
            total += estimate_tokens(tc.result);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Truncate
// ---------------------------------------------------------------------------

void Conversation::truncate_conversation(size_t n) {
    if (n < messages_.size()) {
        messages_.resize(n);
        // Reset next_id_ to maintain the invariant that
        // next_id_ == messages_.size() + 1 (IDs are 1-based).
        // Without this, repeated rollbacks cause ID drift which can
        // make add_tool(message_id - 1) silently fail to match.
        next_id_ = static_cast<int64_t>(n) + 1;
    }
}

// ---------------------------------------------------------------------------
// Compaction support
// ---------------------------------------------------------------------------

void Conversation::replace_with_summary(const std::string& summary) {
    messages_.clear();
    next_id_ = 1;
    appended_system_.clear();
    loaded_skill_names_.clear();

    Message msg;
    msg.role = "user";
    msg.content = "[Conversation summary: " + summary + "]";
    msg.suggested_retention = "preserve";
    messages_.push_back(std::move(msg));

    next_id_ = messages_.size() + 1;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void Conversation::clear() {
    messages_.clear();
    next_id_ = 1;
    appended_system_.clear();
    loaded_skill_names_.clear();
}

// ---------------------------------------------------------------------------
// Serialization
// ---------------------------------------------------------------------------

json Conversation::to_json() const {
    json arr = json::array();
    for (const auto& msg : messages_) {
        json j;
        j["role"] = msg.role;
        if (msg.content.has_value()) {
            j["content"] = *msg.content;
        } else {
            j["content"] = nullptr;
        }
        j["reasoning_content"] = msg.reasoning_content;
        j["tool_call_id"] = msg.tool_call_id;
        j["suggested_retention"] = msg.suggested_retention;

        if (!msg.tool_calls.empty()) {
            json tc_arr = json::array();
            for (const auto& tc : msg.tool_calls) {
                json tc_json;
                tc_json["index"] = tc.index;
                tc_json["id"] = tc.id;
                tc_json["name"] = tc.name;
                tc_json["arguments"] = tc.arguments;
                tc_json["result"] = tc.result;
                tc_arr.push_back(std::move(tc_json));
            }
            j["tool_calls"] = std::move(tc_arr);
        }

        arr.push_back(std::move(j));
    }
    return arr;
}

void Conversation::from_json(const json& j) {
    messages_.clear();
    next_id_ = 1;

    if (!j.is_array())
        return;

    for (const auto& item : j) {
        Message msg;
        msg.role = item.value("role", "");
        auto content_val = item["content"];
        if (content_val.is_null()) {
            msg.content = std::nullopt;
        } else {
            msg.content = content_val.get<std::string>();
        }
        msg.reasoning_content = item.value("reasoning_content", "");
        msg.tool_call_id = item.value("tool_call_id", "");
        msg.suggested_retention = item.value("suggested_retention", "preserve");

        if (item.contains("tool_calls") && item["tool_calls"].is_array()) {
            for (const auto& tc_json : item["tool_calls"]) {
                ToolCall tc;
                tc.index = tc_json.value("index", 0);
                tc.id = tc_json.value("id", "");
                tc.name = tc_json.value("name", "");
                tc.arguments = tc_json.value("arguments", "");
                tc.result = tc_json.value("result", "");
                msg.tool_calls.push_back(std::move(tc));
            }
        }

        messages_.push_back(std::move(msg));
    }

    next_id_ = messages_.size() + 1;
}

Result<void> Conversation::save_to_file(const std::string& path) {
    try {
        auto j = to_json();
        std::ofstream file(path);
        if (!file.is_open()) {
            return std::unexpected("Failed to open " + path + " for writing");
        }
        file << j.dump(2) << std::endl;
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to save conversation: ") + e.what());
    }
}

Result<void> Conversation::load_from_file(const std::string& path) {
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::unexpected("Failed to open " + path + " for reading");
        }
        json j;
        file >> j;
        from_json(j);
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(std::string("Failed to load conversation: ") + e.what());
    }
}
