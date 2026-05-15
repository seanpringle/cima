#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

std::string sanitize_utf8(const std::string& s);

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Usage — token usage reported by the API
// ---------------------------------------------------------------------------

struct Usage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
};

void from_json(const json& j, Usage& u);

// ---------------------------------------------------------------------------
// Display types for the chat UI
// ---------------------------------------------------------------------------
enum class EntryType { UserText, Reasoning, Content, ToolCall };

struct DisplayEntry {
    EntryType type;
    std::string text;
    bool is_streaming = false;
    int seq = 0;
};

// ---------------------------------------------------------------------------
// ToolCall — represents a function call requested by the model
// ---------------------------------------------------------------------------

struct ToolCall {
    int index = 0;
    std::string id;
    std::string name;
    std::string arguments; // accumulated JSON fragment from streaming
    std::string result;    // tool result, empty until filled
};

// ---------------------------------------------------------------------------
// Message — one entry in the conversation history
// ---------------------------------------------------------------------------

struct Message {
    std::string role;                   // system, user, assistant, tool
    std::optional<std::string> content; // null for tool_call messages
    std::string reasoning_content;      // model-specific, may be empty
    std::vector<ToolCall> tool_calls;   // for assistant tool_call msgs
    std::string tool_call_id;           // for tool result messages
    std::string suggested_retention = "preserve";  // "preserve" or "droppable"
};

// ---------------------------------------------------------------------------
// Token estimation — fast approximate token counter
// ---------------------------------------------------------------------------

size_t estimate_tokens(const std::string& text);
size_t estimate_tokens(const Message& msg);

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
    void flush();
    void reset();
    const std::string& raw() const { return raw_; }

  private:
    void process_line(std::string line);

    Callbacks cb_;
    std::string buf_;
    std::string raw_;
};
