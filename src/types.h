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
// TabType — identifies the type of chat session tab
// ---------------------------------------------------------------------------
enum class TabType { Planner, Builder };

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
};

void to_json(json& j, const ToolCall& tc);

// ---------------------------------------------------------------------------
// RetentionClass — how a message is treated during compaction
// ---------------------------------------------------------------------------

enum class RetentionClass : uint8_t {
    Preserve,       // system prompt, user intents, final answers
    Droppable,      // completed tool results, old reasoning, superseded tool calls
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
    RetentionClass retain = RetentionClass::Preserve;
};

// ---------------------------------------------------------------------------
// Token estimation — fast approximate token counter
// ---------------------------------------------------------------------------

size_t estimate_tokens(const std::string& text);
size_t estimate_tokens(const Message& msg);

// ---------------------------------------------------------------------------
// SummaryCallback — used by Conversation::compact to produce summaries
// ---------------------------------------------------------------------------

using SummaryCallback = std::function<std::optional<std::string>(
    const std::vector<Message>& messages, size_t max_tokens)>;

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

// ---------------------------------------------------------------------------
// Conversation — message history with OpenAI-compatible serialization
// ---------------------------------------------------------------------------

class Conversation {
  public:
    explicit Conversation(std::string system_prompt);
    void add_user(std::string content);
    void add_assistant(
        std::string content, std::string reasoning = {}, std::vector<ToolCall> tool_calls = {});
    void add_tool(const std::string& tool_call_id, const std::string& content);
    void set_system_prompt(const std::string& content) { system_prompt_ = content; }
    size_t size() const { return messages_.size(); }
    void truncate(size_t n);
    void clear();
    json to_openai_messages() const;
    const std::string& system_prompt() const { return system_prompt_; }

    // -- Compaction --
    // needs_compaction returns true if estimated total tokens exceed
    // context_limit * compact_threshold_pct / 100.
    // compact unconditionally removes Droppable messages and orphaned
    // assistant tool_calls, then if a summary callback is set, summarizes
    // all remaining messages into a single summary message.
    bool needs_compaction(size_t context_limit, size_t compact_threshold_pct) const;
    void compact();

    void set_summary_callback(SummaryCallback cb) { summary_cb_ = std::move(cb); }
    size_t estimate_total_tokens() const;

  private:
    std::string system_prompt_;
    std::vector<Message> messages_;
    SummaryCallback summary_cb_;

    // Walk backwards from the end and mark superseded tool calls + results as Droppable.
    void mark_superseded_tool_calls();
};
