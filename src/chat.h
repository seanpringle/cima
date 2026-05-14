#pragma once

#include "client.h"
#include "config.h"
#include "plan.h"
#include "session_db.h"
#include "tools.h"
#include "types.h"

#include <nlohmann/json.hpp>

#include <string>

using json = nlohmann::json;

struct ChatResult {
    std::string content;
    std::string reasoning;
};

enum class OutputType { Reasoning, Content, ToolInvocation, Continuation };

using OutputCallback = std::function<void(const std::string& text, OutputType type)>;

class ChatSession {
  public:
    explicit ChatSession(Config config, CancellationToken cancelled = nullptr);

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;
    ChatSession(ChatSession&&) = delete;
    ChatSession& operator=(ChatSession&&) = delete;

    Result<ChatResult> run_once(const std::string& user_input);
    void set_model(const std::string& m) { model_ = m; }
    const std::string& model() const { return model_; }
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }
    const Usage& last_usage() const { return last_usage_; }

    // Each session has its own PlanBoard (not shared across agents).
    PlanBoard& plan() { return plan_; }
    const PlanBoard& plan() const { return plan_; }

    // API connection info (for creating temporary clients in background tasks).
    const std::string& api_base() const { return api_base_; }
    const std::string& api_key() const { return api_key_; }

    // Each session has its own in-memory SQLite database.
    SessionDB& session_db() { return session_db_; }

    // Expose the underlying client so the GUI can call fetch_models() etc.
    ChatClient& client_for_models() { return client_; }

    /// Generate a short, filesystem-safe session title from a conversation.
    /// Makes a lightweight non-streaming API call asking the model to
    /// summarise the conversation topic into 3-5 words.  Returns the title
    /// on success, or an error string on failure.
    Result<std::string> generate_session_title(const std::string& prompt);

    // Return the current safe directory (workspace) path for this session.
    const std::string& safe_dir() const { return *safe_dir_; }

    // Access the continuation slot (used by GUI to display state).
    const ContinuationSlot& continuation_slot() const { return cont_slot_; }

    /// Check metadata thresholds and prepend usage notices to a tool
    /// result string if appropriate (e.g. context >60%, tool-call budget
    /// >90%).  Returns the (potentially modified) result string.
    /// Deduplication is handled via notice_* flags in the metadata table.
    std::string inject_usage_notices(std::string result);

  private:
    std::string model_;
    std::string reasoning_effort_;
    std::shared_ptr<std::string> safe_dir_;
    ContinuationSlot cont_slot_;
    std::string api_base_;     // API base URL (for creating temp clients)
    std::string api_key_;      // API key for authentication
    int max_iterations_ = 100; // overridden by config.max_tool_iterations
    std::string system_prompt_;
    PlanBoard plan_;
    SessionDB session_db_;
    ChatClient client_;
    CancellationToken cancelled_;
    ToolRegistry tools_;
    OutputCallback output_cb_;
    Usage last_usage_;
    int context_limit_ = 300000;           // discovered from API, falls back to Config
    bool context_limit_discovered_ = false;
};

// Free function for generating a session title without requiring a ChatSession instance.
// Takes explicit connection parameters so callers (e.g. GUI background tasks) can pass
// copies without lifetime concerns.
// `conversation` is a vector of alternating user/assistant text messages from
// the session's first exchange, used as context for title generation.
Result<std::string> generate_session_title(const std::string& api_base,
    const std::string& api_key,
    const std::string& model,
    const std::vector<std::string>& conversation);
