#pragma once

#include "client.h"
#include "config.h"
#include "tools.h"
#include "types.h"

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

struct ChatResult {
    std::string content;
    std::string reasoning;
};

enum class OutputType { Reasoning, Content, ToolInvocation };

using OutputCallback = std::function<void(const std::string& text, OutputType type)>;

class ChatSession {
  public:
    explicit ChatSession(Config config);

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;
    ChatSession(ChatSession&&) = delete;
    ChatSession& operator=(ChatSession&&) = delete;

    Result<ChatResult> run_once(const std::string& user_input);
    void set_model(const std::string& m) { model_ = m; }
    void clear();
    void compact();
    const std::string& model() const { return model_; }
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }
    const Usage& last_usage() const { return last_usage_; }

  private:
    std::string model_;
    std::string reasoning_effort_;
    std::string safe_dir_;
    std::string api_key_;        // API key for authentication
    int max_iterations_ = 100;  // overridden by config.max_tool_iterations
    size_t context_limit_;
    size_t compact_threshold_;
    Conversation conversation_;
    ChatClient client_;
    ToolRegistry tools_;
    OutputCallback output_cb_;
    Usage last_usage_;
    bool context_limit_discovered_ = false;

    // Summarization callback for conversation compaction
    std::optional<std::string> summarize_messages_(
        const std::vector<Message>& msgs, size_t max_tokens);
};
