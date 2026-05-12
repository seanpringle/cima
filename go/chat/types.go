package chat

import (
	"sort"
)

// ── Constants ──

// EntryType represents the type of a display entry in the chat UI.
type EntryType int

const (
	EntryUserText  EntryType = iota // User's input text
	EntryReasoning                  // Model's reasoning content (thinking)
	EntryContent                    // Model's response content
	EntryToolCall                   // Tool invocation notification
)

// ── Usage ──

// Usage represents token usage reported by the API.
type Usage struct {
	PromptTokens     int `json:"prompt_tokens"`
	CompletionTokens int `json:"completion_tokens"`
	TotalTokens      int `json:"total_tokens"`
}

// FromJSON populates Usage from a JSON-like map (e.g. from SSE parsing).
func (u *Usage) FromJSON(data map[string]any) {
	if v, ok := data["prompt_tokens"].(float64); ok {
		u.PromptTokens = int(v)
	} else if v, ok := data["prompt_tokens"].(int64); ok {
		u.PromptTokens = int(v)
	}
	if v, ok := data["completion_tokens"].(float64); ok {
		u.CompletionTokens = int(v)
	} else if v, ok := data["completion_tokens"].(int64); ok {
		u.CompletionTokens = int(v)
	}
	if v, ok := data["total_tokens"].(float64); ok {
		u.TotalTokens = int(v)
	} else if v, ok := data["total_tokens"].(int64); ok {
		u.TotalTokens = int(v)
	}
}

// ── ToolCall ──

// ToolCall represents a function call requested by the model.
type ToolCall struct {
	Index     int    `json:"index"`
	ID        string `json:"id"`
	Name      string `json:"name"`
	Arguments string `json:"arguments"` // accumulated JSON fragment from streaming
}

// ── Message ──

// Message represents one entry in the conversation history.
type Message struct {
	Role             string     `json:"role"`                       // system, user, assistant, tool
	Content          *string    `json:"content"`                    // nil for tool_call-only messages
	ReasoningContent string     `json:"reasoning_content,omitempty"` // model-specific, may be empty
	ToolCalls        []ToolCall `json:"tool_calls,omitempty"`       // for assistant tool_call msgs
	ToolCallID       string     `json:"tool_call_id,omitempty"`     // for tool result messages
	Retain           bool       `json:"-"`                          // true = Preserve, false = Droppable
}

// ── DisplayEntry ──

// DisplayEntry represents a UI-visible entry in the chat message list.
type DisplayEntry struct {
	Type        EntryType
	Text        string
	IsStreaming bool
	Seq         int
}

// ── ToolAccumulator ──

// ToolAccumulator merges streaming tool_call deltas across SSE chunks.
type ToolAccumulator struct {
	calls map[int]*ToolCall
}

// Apply processes a delta chunk from an SSE event, accumulating tool call data.
// The delta may contain partial tool_calls fields that are merged by index.
func (a *ToolAccumulator) Apply(delta map[string]any) {
	if a.calls == nil {
		a.calls = make(map[int]*ToolCall)
	}

	tcRaw, ok := delta["tool_calls"]
	if !ok {
		return
	}
	tcArr, ok := tcRaw.([]any)
	if !ok {
		return
	}

	for _, raw := range tcArr {
		item, ok := raw.(map[string]any)
		if !ok {
			continue
		}

		// Get the index (required, always present in streaming chunks)
		idxFloat, ok := item["index"].(float64)
		if !ok {
			continue
		}
		idx := int(idxFloat)

		// Look up or create the call for this index
		call, exists := a.calls[idx]
		if !exists {
			call = &ToolCall{Index: idx}
			a.calls[idx] = call
		}

		// ID (may be in first chunk or a later chunk)
		if id, ok := item["id"].(string); ok && id != "" {
			call.ID = id
		}

		// Function sub-object
		funcRaw, ok := item["function"]
		if !ok {
			continue
		}
		funcObj, ok := funcRaw.(map[string]any)
		if !ok {
			continue
		}

		// Name
		if name, ok := funcObj["name"].(string); ok && name != "" {
			call.Name = name
		}

		// Arguments (accumulated across chunks)
		if args, ok := funcObj["arguments"].(string); ok {
			call.Arguments += args
		}
	}
}

// HasCalls returns true if at least one tool call has been accumulated.
func (a *ToolAccumulator) HasCalls() bool {
	return len(a.calls) > 0
}

// Finalize returns all accumulated tool calls, sorted by index.
func (a *ToolAccumulator) Finalize() []ToolCall {
	result := make([]ToolCall, 0, len(a.calls))
	for _, call := range a.calls {
		result = append(result, *call)
	}
	// Sort by index to maintain the order the model intended
	sort.Slice(result, func(i, j int) bool {
		return result[i].Index < result[j].Index
	})
	return result
}
