#pragma once

#include "config.h"
#include "types.h"

#include <string>
#include <vector>

/// In-memory conversation history, stored as a vector of Message structs.
/// Replaces the old approach of storing messages in a SQLite `messages` table.
class Conversation {
  public:
    Conversation() = default;

    // ── Message management ──

    /// Add a user message. Returns the new message id.
    int64_t add_user(const std::string& content);

    /// Add a system message. Returns the new message id.
    int64_t add_system(const std::string& content,
        const std::string& suggested_retention = "droppable");

    /// Add an assistant message. If tool_calls is non-empty, the message
    /// content is set to nullopt and the calls are stored on the message.
    /// Returns the new message id.
    int64_t add_assistant(const std::string& content,
        const std::string& reasoning = {},
        const std::vector<ToolCall>& tool_calls = {});

    /// Add a notice message (user role, droppable suggested_retention).
    int64_t add_notice(const std::string& content);

    /// Set the result of a tool call on the given assistant message.
    void add_tool(int64_t message_id,
        const std::string& tool_call_id,
        const std::string& content);

    /// Build the OpenAI-compatible messages array.
    json build_openai_payload(const std::string& system_prompt) const;

    /// Estimate total tokens for all messages.
    size_t estimate_total_tokens() const;

    /// Estimate tokens for compactable content (tool results).
    size_t estimate_droppable_tokens() const;

    /// Return the number of messages in the conversation.
    size_t message_count() const { return messages_.size(); }

    /// Truncate conversation to at most N messages (for rollback on error).
    void truncate_conversation(size_t n);

    /// Access the raw messages.
    std::vector<Message>& messages() { return messages_; }
    const std::vector<Message>& messages() const { return messages_; }

    /// Replace the entire conversation with a single summary message.
    void replace_with_summary(const std::string& summary);

    // ── Serialization ──

    json to_json() const;
    void from_json(const json& j);

    Result<void> save_to_file(const std::string& path);
    Result<void> load_from_file(const std::string& path);

  private:
    std::vector<Message> messages_;
    int64_t next_id_ = 1;
};
