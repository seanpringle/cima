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
    void clear();
    void compact();
    const std::string& model() const { return model_; }
    void set_output_callback(OutputCallback cb) { output_cb_ = std::move(cb); }
    const Usage& last_usage() const { return last_usage_; }

    // Each session has its own PlanBoard (not shared across agents).
    PlanBoard& plan() { return plan_; }
    const PlanBoard& plan() const { return plan_; }

    // Each session has its own in-memory SQLite database.
    SessionDB& session_db() { return session_db_; }

    // Expose the underlying client so the GUI can call fetch_models() etc.
    ChatClient& client_for_models() { return client_; }

    // Return the current safe directory (workspace) path for this session.
    // This can change over time (e.g. via worktree tools).
    const std::string& safe_dir() const { return *safe_dir_; }

    // Access the continuation slot (used by GUI to display state).
    const ContinuationSlot& continuation_slot() const { return cont_slot_; }

  private:
    std::string model_;
    std::string reasoning_effort_;
    std::shared_ptr<std::string> safe_dir_;
    ContinuationSlot cont_slot_;
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
    bool context_limit_discovered_ = false;
};
