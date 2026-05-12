package chat

import (
	"testing"
)

// ── ToolAccumulator tests ──

func TestToolAccumulatorSingleChunk(t *testing.T) {
	var acc ToolAccumulator
	delta := map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"id":    "call_abc",
				"type":  "function",
				"function": map[string]any{
					"name":      "list_files",
					"arguments": `{"path": "/tmp"}`,
				},
			},
		},
	}
	acc.Apply(delta)

	if !acc.HasCalls() {
		t.Fatal("expected HasCalls() = true")
	}
	calls := acc.Finalize()
	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d", len(calls))
	}
	if calls[0].Index != 0 {
		t.Errorf("Index = %d, want 0", calls[0].Index)
	}
	if calls[0].ID != "call_abc" {
		t.Errorf("ID = %q, want %q", calls[0].ID, "call_abc")
	}
	if calls[0].Name != "list_files" {
		t.Errorf("Name = %q, want %q", calls[0].Name, "list_files")
	}
	if calls[0].Arguments != `{"path": "/tmp"}` {
		t.Errorf("Arguments = %q, want %q", calls[0].Arguments, `{"path": "/tmp"}`)
	}
}

func TestToolAccumulatorMultiChunk(t *testing.T) {
	var acc ToolAccumulator

	// Chunk 1: id + name + empty args
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"id":    "call_xyz",
				"function": map[string]any{
					"name":      "read_file",
					"arguments": "",
				},
			},
		},
	})

	// Chunk 2: partial args fragment
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"function": map[string]any{
					"arguments": `{"path":`,
				},
			},
		},
	})

	// Chunk 3: rest of args
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"function": map[string]any{
					"arguments": ` "/etc/hosts"}`,
				},
			},
		},
	})

	if !acc.HasCalls() {
		t.Fatal("expected HasCalls() = true")
	}
	calls := acc.Finalize()
	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d", len(calls))
	}
	if calls[0].ID != "call_xyz" {
		t.Errorf("ID = %q, want %q", calls[0].ID, "call_xyz")
	}
	if calls[0].Name != "read_file" {
		t.Errorf("Name = %q, want %q", calls[0].Name, "read_file")
	}
	if calls[0].Arguments != `{"path": "/etc/hosts"}` {
		t.Errorf("Arguments = %q, want %q", calls[0].Arguments, `{"path": "/etc/hosts"}`)
	}
}

func TestToolAccumulatorNoToolCalls(t *testing.T) {
	var acc ToolAccumulator
	delta := map[string]any{
		"content": "hello",
	}
	acc.Apply(delta)
	if acc.HasCalls() {
		t.Error("HasCalls should be false when no tool_calls in delta")
	}
}

func TestToolAccumulatorEmptyDelta(t *testing.T) {
	var acc ToolAccumulator
	delta := map[string]any{}
	acc.Apply(delta)
	if acc.HasCalls() {
		t.Error("HasCalls should be false for empty delta")
	}
}

func TestToolAccumulatorMultipleParallel(t *testing.T) {
	var acc ToolAccumulator

	// Chunk with two tool call starts
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"id":    "call_1",
				"function": map[string]any{
					"name":      "list_files",
					"arguments": `{"pa`,
				},
			},
			map[string]any{
				"index": float64(1),
				"id":    "call_2",
				"function": map[string]any{
					"name":      "read_file",
					"arguments": `{"pat`,
				},
			},
		},
	})

	// Chunk with continued args
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"function": map[string]any{
					"arguments": `th": "/tmp"}`,
				},
			},
			map[string]any{
				"index": float64(1),
				"function": map[string]any{
					"arguments": `h": "/etc/hosts"}`,
				},
			},
		},
	})

	calls := acc.Finalize()
	if len(calls) != 2 {
		t.Fatalf("expected 2 calls, got %d", len(calls))
	}

	// Finalize returns calls sorted by index
	if calls[0].Index != 0 {
		t.Errorf("calls[0].Index = %d, want 0", calls[0].Index)
	}
	if calls[0].ID != "call_1" {
		t.Errorf("calls[0].ID = %q, want %q", calls[0].ID, "call_1")
	}
	if calls[0].Name != "list_files" {
		t.Errorf("calls[0].Name = %q, want %q", calls[0].Name, "list_files")
	}
	if calls[0].Arguments != `{"path": "/tmp"}` {
		t.Errorf("calls[0].Arguments = %q", calls[0].Arguments)
	}

	if calls[1].Index != 1 {
		t.Errorf("calls[1].Index = %d, want 1", calls[1].Index)
	}
	if calls[1].ID != "call_2" {
		t.Errorf("calls[1].ID = %q, want %q", calls[1].ID, "call_2")
	}
	if calls[1].Name != "read_file" {
		t.Errorf("calls[1].Name = %q, want %q", calls[1].Name, "read_file")
	}
	if calls[1].Arguments != `{"path": "/etc/hosts"}` {
		t.Errorf("calls[1].Arguments = %q", calls[1].Arguments)
	}
}

func TestToolAccumulatorFinalizeOrder(t *testing.T) {
	var acc ToolAccumulator

	// Add in reverse index order to test sorting
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(1),
				"id":    "call_2",
				"function": map[string]any{
					"name": "tool_b",
				},
			},
			map[string]any{
				"index": float64(0),
				"id":    "call_1",
				"function": map[string]any{
					"name": "tool_a",
				},
			},
		},
	})

	calls := acc.Finalize()
	if len(calls) != 2 {
		t.Fatalf("expected 2 calls, got %d", len(calls))
	}
	if calls[0].Index != 0 || calls[0].Name != "tool_a" {
		t.Errorf("calls[0] = {Index: %d, Name: %s}, want {Index: 0, Name: tool_a}", calls[0].Index, calls[0].Name)
	}
	if calls[1].Index != 1 || calls[1].Name != "tool_b" {
		t.Errorf("calls[1] = {Index: %d, Name: %s}, want {Index: 1, Name: tool_b}", calls[1].Index, calls[1].Name)
	}
}

func TestToolAccumulatorIDInSecondChunk(t *testing.T) {
	// Some implementations send the ID in a chunk that also has function data
	// but only the index is present in the first chunk.
	var acc ToolAccumulator

	// First chunk: only index and function name
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"function": map[string]any{
					"name": "my_tool",
				},
			},
		},
	})

	// Second chunk: id and arguments
	acc.Apply(map[string]any{
		"tool_calls": []any{
			map[string]any{
				"index": float64(0),
				"id":    "call_final",
				"function": map[string]any{
					"arguments": `{}`,
				},
			},
		},
	})

	calls := acc.Finalize()
	if len(calls) != 1 {
		t.Fatalf("expected 1 call, got %d", len(calls))
	}
	if calls[0].ID != "call_final" {
		t.Errorf("ID = %q, want %q", calls[0].ID, "call_final")
	}
	if calls[0].Name != "my_tool" {
		t.Errorf("Name = %q, want %q", calls[0].Name, "my_tool")
	}
	if calls[0].Arguments != `{}` {
		t.Errorf("Arguments = %q, want %q", calls[0].Arguments, `{}`)
	}
}

func TestToolAccumulatorNonArrayToolCalls(t *testing.T) {
	// Some deltas might have tool_calls as non-array; should be ignored
	var acc ToolAccumulator
	delta := map[string]any{
		"tool_calls": "not_an_array",
	}
	acc.Apply(delta)
	if acc.HasCalls() {
		t.Error("HasCalls should be false when tool_calls is not an array")
	}
}

// ── DisplayEntry tests ──

func TestDisplayEntryCreation(t *testing.T) {
	e := DisplayEntry{
		Type:        EntryContent,
		Text:        "Hello",
		IsStreaming: true,
		Seq:         1,
	}
	if e.Type != EntryContent {
		t.Errorf("Type = %v, want EntryContent", e.Type)
	}
	if e.Text != "Hello" {
		t.Errorf("Text = %q, want %q", e.Text, "Hello")
	}
	if !e.IsStreaming {
		t.Error("IsStreaming should be true")
	}
	if e.Seq != 1 {
		t.Errorf("Seq = %d, want 1", e.Seq)
	}
}

func TestDisplayEntryTypes(t *testing.T) {
	entries := []DisplayEntry{
		{Type: EntryUserText},
		{Type: EntryReasoning},
		{Type: EntryContent},
		{Type: EntryToolCall},
	}
	if len(entries) != 4 {
		t.Fatalf("expected 4 entry types")
	}
	// Just ensure they are distinct
	seen := map[EntryType]bool{}
	for _, e := range entries {
		if seen[e.Type] {
			t.Errorf("duplicate EntryType %v", e.Type)
		}
		seen[e.Type] = true
	}
}

func TestDisplayEntrySeqIncrement(t *testing.T) {
	e1 := DisplayEntry{Type: EntryUserText, Text: "a", Seq: 1}
	e2 := DisplayEntry{Type: EntryContent, Text: "b", Seq: 2}
	if e2.Seq <= e1.Seq {
		t.Error("Seq should increment")
	}
}

// ── Usage tests ──

func TestUsageFromJSON(t *testing.T) {
	data := map[string]any{
		"prompt_tokens":     int64(50),
		"completion_tokens": int64(100),
		"total_tokens":      int64(150),
	}
	var u Usage
	u.FromJSON(data)
	if u.PromptTokens != 50 {
		t.Errorf("PromptTokens = %d, want 50", u.PromptTokens)
	}
	if u.CompletionTokens != 100 {
		t.Errorf("CompletionTokens = %d, want 100", u.CompletionTokens)
	}
	if u.TotalTokens != 150 {
		t.Errorf("TotalTokens = %d, want 150", u.TotalTokens)
	}
}

func TestUsageFromJSONMissingFields(t *testing.T) {
	data := map[string]any{}
	var u Usage
	u.FromJSON(data)
	if u.PromptTokens != 0 {
		t.Errorf("PromptTokens = %d, want 0", u.PromptTokens)
	}
	if u.CompletionTokens != 0 {
		t.Errorf("CompletionTokens = %d, want 0", u.CompletionTokens)
	}
	if u.TotalTokens != 0 {
		t.Errorf("TotalTokens = %d, want 0", u.TotalTokens)
	}
}

// ── Message tests ──

func TestMessageContent(t *testing.T) {
	content := "Hello, world!"
	msg := Message{
		Role:    "user",
		Content: &content,
	}
	if msg.Role != "user" {
		t.Errorf("Role = %q, want %q", msg.Role, "user")
	}
	if msg.Content == nil || *msg.Content != "Hello, world!" {
		t.Errorf("Content = %v, want %q", msg.Content, "Hello, world!")
	}
}

func TestMessageNilContent(t *testing.T) {
	// Tool-call-only assistant messages have nil content
	msg := Message{
		Role:      "assistant",
		Content:   nil,
		ToolCalls: []ToolCall{{ID: "call_1", Name: "list_files"}},
	}
	if msg.Content != nil {
		t.Error("Content should be nil for tool-call-only message")
	}
	if len(msg.ToolCalls) != 1 {
		t.Errorf("ToolCalls len = %d, want 1", len(msg.ToolCalls))
	}
}

func TestMessageToolResult(t *testing.T) {
	content := "file1.txt"
	msg := Message{
		Role:       "tool",
		Content:    &content,
		ToolCallID: "call_1",
	}
	if msg.ToolCallID != "call_1" {
		t.Errorf("ToolCallID = %q, want %q", msg.ToolCallID, "call_1")
	}
}
