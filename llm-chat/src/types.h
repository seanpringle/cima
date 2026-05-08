#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// ToolCall — represents a function call requested by the model
// ---------------------------------------------------------------------------

struct ToolCall {
    int index = 0;
    std::string id;
    std::string name;
    std::string arguments;  // accumulated JSON fragment from streaming
};

void to_json(json& j, const ToolCall& tc);

// ---------------------------------------------------------------------------
// Message — one entry in the conversation history
// ---------------------------------------------------------------------------

struct Message {
    std::string role;                           // system, user, assistant, tool
    std::optional<std::string> content;         // null for tool_call messages
    std::string reasoning_content;              // model-specific, may be empty
    std::optional<ToolCall> tool_call;          // for assistant tool_call msgs
    std::string tool_call_id;                   // for tool result messages
};

// ---------------------------------------------------------------------------
// ToolAccumulator — merges streaming tool_call deltas across SSE chunks
// ---------------------------------------------------------------------------

class ToolAccumulator {
public:
    void apply(const json& delta);
    bool has_calls() const { return !calls_.empty(); }
    std::vector<ToolCall> finalize() const;

private:
    std::unordered_map<int, ToolCall> calls_;
};

// ---------------------------------------------------------------------------
// SSEParser — incremental SSE line parser for curl write callback
// ---------------------------------------------------------------------------

class SSEParser {
public:
    using DataCallback = std::function<void(const json&)>;
    using DoneCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    struct Callbacks {
        DataCallback on_data;
        DoneCallback on_done;
        ErrorCallback on_error;
    };

    explicit SSEParser(Callbacks cb);
    void feed(const char* data, size_t len);
    void reset();

private:
    void process_line(std::string line);

    Callbacks cb_;
    std::string buf_;
};

// ---------------------------------------------------------------------------
// Conversation — message history with OpenAI-compatible serialization
// ---------------------------------------------------------------------------

class Conversation {
public:
    explicit Conversation(std::string system_prompt);
    void add_user(std::string content);
    void add_assistant(std::string content, std::string reasoning = {},
                       std::vector<ToolCall> tool_calls = {});
    void add_tool(const std::string& tool_call_id, const std::string& content);
    void clear();
    json to_openai_messages() const;
    const std::string& system_prompt() const { return system_prompt_; }

private:
    std::string system_prompt_;
    std::vector<Message> messages_;
};
