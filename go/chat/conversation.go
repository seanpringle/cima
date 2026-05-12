package chat

import (
	"strings"
)

// ── Token estimation ──

// estimateTokens returns a fast approximate GPT token count for a string.
// Uses the formula: (len(text) + 3) / 4 + 1
func estimateTokens(text string) int {
	if len(text) == 0 {
		return 1
	}
	return (len(text) + 3) / 4 + 1
}

// estimateMessageTokens estimates the token count for a single message including overhead.
func estimateMessageTokens(msg Message) int {
	t := 0
	if msg.Content != nil {
		t += estimateTokens(*msg.Content)
	}
	t += estimateTokens(msg.ReasoningContent)
	for _, tc := range msg.ToolCalls {
		t += estimateTokens(tc.ID)
		t += estimateTokens(tc.Name)
		t += estimateTokens(tc.Arguments)
	}
	t += estimateTokens(msg.ToolCallID)
	// JSON framing overhead per message (~20 tokens for role + surrounding keys)
	t += 20
	return t
}

// ── SummaryCallback ──

// SummaryCallback produces a summary of a set of messages within a token budget.
// Returns nil if summarization is not possible.
type SummaryCallback func(messages []Message, maxTokens int) *string

// ── Conversation ──

// Conversation manages the message history for a chat session.
// It supports adding user/assistant/tool messages, serializing to OpenAI format,
// estimating token usage, and compaction of old messages.
type Conversation struct {
	systemPrompt string
	messages     []Message
	summaryCb    SummaryCallback
}

// NewConversation creates a new conversation with the given system prompt.
func NewConversation(systemPrompt string) *Conversation {
	return &Conversation{
		systemPrompt: systemPrompt,
		messages:     make([]Message, 0),
	}
}

// AddUser appends a user message with Preserve retention.
func (c *Conversation) AddUser(content string) {
	c.messages = append(c.messages, Message{
		Role:    "user",
		Content: &content,
		Retain:  true,
	})
}

// AddAssistant appends an assistant message.
// If toolCalls is non-empty, this is a tool-call-only message (content is nil).
// If content is non-empty, this is a final answer message.
// Before adding a content-bearing message, previous tool-call rounds are marked as droppable.
func (c *Conversation) AddAssistant(content, reasoning string, toolCalls []ToolCall) {
	// If this is a content-bearing response, mark previous tool-call rounds as droppable
	if content != "" {
		c.markSupersededToolCalls()
	}

	msg := Message{
		Role:             "assistant",
		ReasoningContent: reasoning,
	}

	if len(toolCalls) > 0 {
		// Tool-call-only message: content is nil
		msg.Content = nil
		msg.ToolCalls = toolCalls
	} else {
		msg.Content = &content
		msg.Retain = true
	}

	c.messages = append(c.messages, msg)
}

// AddTool appends a tool result message with Droppable retention.
func (c *Conversation) AddTool(toolCallID, content string) {
	contentCopy := content
	c.messages = append(c.messages, Message{
		Role:       "tool",
		Content:    &contentCopy,
		ToolCallID: toolCallID,
		Retain:     false, // Droppable
	})
}

// SetSystemPrompt updates the system prompt.
func (c *Conversation) SetSystemPrompt(prompt string) {
	c.systemPrompt = prompt
}

// SystemPrompt returns the current system prompt.
func (c *Conversation) SystemPrompt() string {
	return c.systemPrompt
}

// Size returns the number of messages in the conversation (excluding system prompt).
func (c *Conversation) Size() int {
	return len(c.messages)
}

// Truncate truncates the message list to n messages.
func (c *Conversation) Truncate(n int) {
	if n < len(c.messages) {
		c.messages = c.messages[:n]
	}
}

// Clear removes all messages but preserves the system prompt.
func (c *Conversation) Clear() {
	c.messages = nil
}

// ToOpenAIMessages serializes the conversation to OpenAI-compatible message format.
// The first element is always the system message.
func (c *Conversation) ToOpenAIMessages() []map[string]any {
	result := make([]map[string]any, 0, 1+len(c.messages))

	// System message
	result = append(result, map[string]any{
		"role":    "system",
		"content": c.systemPrompt,
	})

	// Conversation messages
	for _, msg := range c.messages {
		entry := map[string]any{
			"role": msg.Role,
		}

		if msg.Role == "assistant" && len(msg.ToolCalls) > 0 {
			// Tool-call-only message
			if msg.ReasoningContent != "" {
				entry["reasoning_content"] = msg.ReasoningContent
			}
			entry["content"] = nil
			tcList := make([]any, 0, len(msg.ToolCalls))
			for _, tc := range msg.ToolCalls {
				tcList = append(tcList, map[string]any{
					"id":   sanitizeString(tc.ID),
					"type": "function",
					"function": map[string]any{
						"name":      sanitizeString(tc.Name),
						"arguments": sanitizeString(tc.Arguments),
					},
				})
			}
			entry["tool_calls"] = tcList
		} else {
			// Regular or tool message
			if msg.Content != nil {
				entry["content"] = *msg.Content
			} else {
				entry["content"] = nil
			}
			if msg.Role == "assistant" && msg.ReasoningContent != "" {
				entry["reasoning_content"] = msg.ReasoningContent
			}
			if msg.Role == "tool" {
				entry["tool_call_id"] = sanitizeString(msg.ToolCallID)
			}
		}

		result = append(result, entry)
	}

	return result
}

// EstimateTotalTokens returns an approximate token count for the entire conversation.
func (c *Conversation) EstimateTotalTokens() int {
	t := estimateTokens(c.systemPrompt) + 20
	for _, msg := range c.messages {
		t += estimateMessageTokens(msg)
	}
	return t
}

// NeedsCompaction returns true if the estimated token count exceeds the threshold.
func (c *Conversation) NeedsCompaction(contextLimit int, thresholdPct int) bool {
	if contextLimit <= 0 {
		return false
	}
	threshold := contextLimit * thresholdPct / 100
	return c.EstimateTotalTokens() > threshold
}

// Compact removes droppable messages and orphaned tool-call assistant messages.
// If a SummaryCallback is set, the remaining messages are summarized into a single message.
func (c *Conversation) Compact() {
	// 1. Remove all Droppable messages
	filtered := make([]Message, 0, len(c.messages))
	for _, msg := range c.messages {
		if msg.Retain {
			filtered = append(filtered, msg)
		}
	}
	c.messages = filtered

	// 2. Remove orphaned assistant tool-call messages (no matching tool result)
	activeToolIDs := make(map[string]bool)
	for _, msg := range c.messages {
		if msg.Role == "tool" && msg.ToolCallID != "" {
			activeToolIDs[msg.ToolCallID] = true
		}
	}

	filtered = make([]Message, 0, len(c.messages))
	for _, msg := range c.messages {
		if msg.Role == "assistant" && len(msg.ToolCalls) > 0 {
			anyAlive := false
			for _, tc := range msg.ToolCalls {
				if activeToolIDs[tc.ID] {
					anyAlive = true
					break
				}
			}
			if !anyAlive {
				continue
			}
		}
		filtered = append(filtered, msg)
	}
	c.messages = filtered

	// 3. If summary callback is set, summarize remaining messages
	if c.summaryCb != nil && len(c.messages) > 0 {
		summaryBudget := 10000
		summary := c.summaryCb(c.messages, summaryBudget)
		if summary != nil {
			summaryText := "[Previous exchanges summarized: " + *summary + "]"
			c.messages = []Message{
				{
					Role:    "user",
					Content: &summaryText,
					Retain:  true,
				},
			}
		}
	}
}

// SetSummaryCallback sets the callback for conversation compaction summarization.
func (c *Conversation) SetSummaryCallback(cb SummaryCallback) {
	c.summaryCb = cb
}

// markSupersededToolCalls walks backwards from the end of the message list.
// When it finds a content-bearing assistant message (final answer), everything
// before it that's a tool-call round is marked as Droppable.
func (c *Conversation) markSupersededToolCalls() {
	foundContent := false

	// Walk backwards
	for i := len(c.messages) - 1; i >= 0; i-- {
		msg := &c.messages[i]
		if msg.Role == "assistant" && msg.Content != nil && *msg.Content != "" {
			foundContent = true
			continue
		}
		if !foundContent {
			continue
		}
		if msg.Role == "assistant" && len(msg.ToolCalls) > 0 {
			// Mark corresponding tool results as Droppable
			for _, tc := range msg.ToolCalls {
				for j := range c.messages {
					if c.messages[j].Role == "tool" && c.messages[j].ToolCallID == tc.ID && c.messages[j].Retain {
						c.messages[j].Retain = false // Droppable
					}
				}
			}
		}
	}
}

// sanitizeString ensures the output contains valid UTF-8.
// In Go, strings are already valid UTF-8 (or at least contain valid byte sequences),
// so this is mostly a no-op for safety.
func sanitizeString(s string) string {
	// Go strings can contain arbitrary bytes; for API safety we ensure
	// the string is valid UTF-8 by replacing invalid sequences.
	// This is rarely needed in practice since most strings come from JSON.
	return strings.ToValidUTF8(s, string("\ufffd"))
}


