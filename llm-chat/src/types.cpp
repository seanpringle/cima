#include "types.h"

// ---------------------------------------------------------------------------
// ToolCall JSON serialization
// ---------------------------------------------------------------------------

void to_json(json& j, const ToolCall& tc) {
    j = json{
        {"index", tc.index},
        {"id", tc.id},
        {"name", tc.name},
        {"arguments", tc.arguments},
    };
}

// ---------------------------------------------------------------------------
// ToolAccumulator
// ---------------------------------------------------------------------------

void ToolAccumulator::apply(const json& delta) {
    auto it = delta.find("tool_calls");
    if (it == delta.end() || !it->is_array()) {
        return;
    }

    for (const auto& tc : *it) {
        int idx = tc.value("index", 0);
        auto& call = calls_[idx];
        call.index = idx;

        auto id_it = tc.find("id");
        if (id_it != tc.end() && id_it->is_string()) {
            call.id = id_it->get<std::string>();
        }

        auto func_it = tc.find("function");
        if (func_it != tc.end() && func_it->is_object()) {
            auto name_it = func_it->find("name");
            if (name_it != func_it->end() && name_it->is_string()) {
                call.name = name_it->get<std::string>();
            }
            auto args_it = func_it->find("arguments");
            if (args_it != func_it->end() && args_it->is_string()) {
                call.arguments += args_it->get<std::string>();
            }
        }
    }
}

std::vector<ToolCall> ToolAccumulator::finalize() const {
    std::vector<ToolCall> result;
    result.reserve(calls_.size());
    for (const auto& [idx, tc] : calls_) {
        result.push_back(tc);
    }
    return result;
}

// ---------------------------------------------------------------------------
// SSEParser
// ---------------------------------------------------------------------------

SSEParser::SSEParser(Callbacks cb) : cb_(std::move(cb)) {}

void SSEParser::feed(const char* data, size_t len) {
    buf_.append(data, len);

    while (true) {
        auto nl = buf_.find('\n');
        if (nl == std::string::npos) {
            break;  // incomplete line, wait for more data
        }

        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1);  // remove line + \n
        process_line(std::move(line));
    }
}

void SSEParser::process_line(std::string line) {
    // Strip trailing \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    // Skip empty lines (event separator)
    if (line.empty()) {
        return;
    }

    constexpr std::string_view prefix = "data: ";
    if (line.size() > prefix.size() &&
        std::string_view(line).substr(0, prefix.size()) == prefix) {
        std::string payload = line.substr(prefix.size());

        if (payload == "[DONE]") {
            if (cb_.on_done) {
                cb_.on_done();
            }
            return;
        }

        try {
            json j = json::parse(payload);
            if (cb_.on_data) {
                cb_.on_data(j);
            }
        } catch (const json::parse_error& e) {
            if (cb_.on_error) {
                cb_.on_error(std::string("JSON parse error: ") + e.what() +
                             " | payload: " + payload);
            }
        }
        return;
    }

    // Ignore other fields (event:, :keepalive, etc.)
}

void SSEParser::reset() {
    buf_.clear();
}

// ---------------------------------------------------------------------------
// Conversation
// ---------------------------------------------------------------------------

Conversation::Conversation(std::string system_prompt)
    : system_prompt_(std::move(system_prompt)) {}

void Conversation::add_user(std::string content) {
    messages_.push_back(
        Message{.role = "user", .content = std::move(content)});
}

void Conversation::add_assistant(std::string content, std::string reasoning,
                                 std::vector<ToolCall> tool_calls) {
    Message msg;
    msg.role = "assistant";
    msg.reasoning_content = std::move(reasoning);

    if (!tool_calls.empty()) {
        msg.content = std::nullopt;
        msg.tool_call = std::move(tool_calls.front());
    } else {
        msg.content = std::move(content);
    }

    messages_.push_back(std::move(msg));
}

void Conversation::add_tool(const std::string& tool_call_id,
                            const std::string& content) {
    messages_.push_back(Message{
        .role = "tool", .content = content, .tool_call_id = tool_call_id});
}

void Conversation::clear() {
    messages_.clear();
}

json Conversation::to_openai_messages() const {
    json arr = json::array();

    arr.push_back({{"role", "system"}, {"content", system_prompt_}});

    for (const auto& msg : messages_) {
        json j;
        j["role"] = msg.role;

        if (msg.role == "assistant" && msg.tool_call.has_value()) {
            j["content"] = nullptr;
            json tc;
            tc["id"] = msg.tool_call->id;
            tc["type"] = "function";
            tc["function"]["name"] = msg.tool_call->name;
            tc["function"]["arguments"] = msg.tool_call->arguments;
            j["tool_calls"] = json::array({std::move(tc)});

            // always include reasoning_content for tool_call messages
            j["reasoning_content"] = msg.reasoning_content;
        } else {
            j["content"] = msg.content.value_or("");

            if (!msg.reasoning_content.empty()) {
                j["reasoning_content"] = msg.reasoning_content;
            }
        }

        if (!msg.tool_call_id.empty()) {
            j["tool_call_id"] = msg.tool_call_id;
        }

        arr.push_back(std::move(j));
    }

    return arr;
}
