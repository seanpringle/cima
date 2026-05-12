package chat

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestBasicExchange(t *testing.T) {
	conv := NewConversation("You are helpful.")
	conv.AddUser("Hello")
	conv.AddAssistant("Hi there!", "", nil)

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 messages, got %d", len(msgs))
	}
	if msgs[0]["role"] != "system" {
		t.Errorf("msg[0] role = %v, want 'system'", msgs[0]["role"])
	}
	if msgs[0]["content"] != "You are helpful." {
		t.Errorf("msg[0] content = %v", msgs[0]["content"])
	}
	if msgs[1]["role"] != "user" {
		t.Errorf("msg[1] role = %v", msgs[1]["role"])
	}
	if msgs[1]["content"] != "Hello" {
		t.Errorf("msg[1] content = %v", msgs[1]["content"])
	}
	if msgs[2]["role"] != "assistant" {
		t.Errorf("msg[2] role = %v", msgs[2]["role"])
	}
	if msgs[2]["content"] != "Hi there!" {
		t.Errorf("msg[2] content = %v", msgs[2]["content"])
	}
}

func TestWithReasoning(t *testing.T) {
	conv := NewConversation("Be brief.")
	conv.AddUser("What is 2+2?")
	conv.AddAssistant("4", "The user asked a simple arithmetic question.", nil)

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 messages, got %d", len(msgs))
	}
	if msgs[2]["content"] != "4" {
		t.Errorf("content = %v, want '4'", msgs[2]["content"])
	}
	if msgs[2]["reasoning_content"] != "The user asked a simple arithmetic question." {
		t.Errorf("reasoning_content = %v", msgs[2]["reasoning_content"])
	}
}

func TestWithToolCalls(t *testing.T) {
	conv := NewConversation("You are helpful.")
	conv.AddUser("List files in /tmp")

	tc := ToolCall{Index: 0, ID: "call_abc123", Name: "list_files", Arguments: `{"path": "/tmp"}`}
	conv.AddAssistant("", "Need to list /tmp contents.", []ToolCall{tc})

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 messages, got %d", len(msgs))
	}

	// Content should be nil for tool-call-only assistant messages
	if msgs[2]["content"] != nil {
		t.Errorf("content should be nil for tool_call message, got %v", msgs[2]["content"])
	}
	if msgs[2]["reasoning_content"] != "Need to list /tmp contents." {
		t.Errorf("reasoning_content = %v", msgs[2]["reasoning_content"])
	}

	// Check tool_calls array
	tcs, ok := msgs[2]["tool_calls"].([]any)
	if !ok {
		t.Fatal("tool_calls should be an array")
	}
	if len(tcs) != 1 {
		t.Fatalf("expected 1 tool call, got %d", len(tcs))
	}
	tcMap := tcs[0].(map[string]any)
	if tcMap["id"] != "call_abc123" {
		t.Errorf("tool_call id = %v", tcMap["id"])
	}
	if tcMap["type"] != "function" {
		t.Errorf("tool_call type = %v", tcMap["type"])
	}
	funcMap := tcMap["function"].(map[string]any)
	if funcMap["name"] != "list_files" {
		t.Errorf("function name = %v", funcMap["name"])
	}
	if funcMap["arguments"] != `{"path": "/tmp"}` {
		t.Errorf("function arguments = %v", funcMap["arguments"])
	}
}

func TestWithToolResult(t *testing.T) {
	conv := NewConversation("You are helpful.")
	conv.AddUser("List files")

	tc := ToolCall{Index: 0, ID: "call_req1", Name: "list_files", Arguments: `{"path": "."}`}
	conv.AddAssistant("", "", []ToolCall{tc})
	conv.AddTool("call_req1", "file1.txt\nfile2.txt")

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 4 {
		t.Fatalf("expected 4 messages, got %d", len(msgs))
	}

	// Tool result message
	if msgs[3]["role"] != "tool" {
		t.Errorf("msg[3] role = %v, want 'tool'", msgs[3]["role"])
	}
	if msgs[3]["tool_call_id"] != "call_req1" {
		t.Errorf("tool_call_id = %v", msgs[3]["tool_call_id"])
	}
	if msgs[3]["content"] != "file1.txt\nfile2.txt" {
		t.Errorf("content = %v", msgs[3]["content"])
	}
}

func TestClearPreservesSystem(t *testing.T) {
	conv := NewConversation("System prompt.")
	conv.AddUser("Hello")
	conv.AddAssistant("Hi", "", nil)
	conv.Clear()

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 1 {
		t.Fatalf("expected 1 message (system) after clear, got %d", len(msgs))
	}
	if msgs[0]["role"] != "system" {
		t.Errorf("msg[0] role = %v, want 'system'", msgs[0]["role"])
	}
	if msgs[0]["content"] != "System prompt." {
		t.Errorf("msg[0] content = %v", msgs[0]["content"])
	}
}

func TestToOpenAIMessagesFormat(t *testing.T) {
	conv := NewConversation("You are a bot.")
	conv.AddUser("Hi")
	conv.AddAssistant("Hello!", "", nil)

	msgs := conv.ToOpenAIMessages()
	data, err := json.Marshal(msgs)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}

	var parsed []map[string]any
	if err := json.Unmarshal(data, &parsed); err != nil {
		t.Fatalf("json.Unmarshal: %v", err)
	}
	if len(parsed) != 3 {
		t.Fatalf("expected 3, got %d", len(parsed))
	}
}

func TestReasoningContentOmittedWhenEmpty(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hi")
	conv.AddAssistant("hello", "", nil)

	msgs := conv.ToOpenAIMessages()
	// reasoning_content should not be in the JSON when empty
	data, _ := json.Marshal(msgs[2])
	if strings.Contains(string(data), "reasoning_content") {
		t.Errorf("reasoning_content should be omitted when empty, got: %s", data)
	}
}

func TestEstimateTokens(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hello world")
	conv.AddAssistant("hi there", "", nil)

	tokens := conv.EstimateTotalTokens()
	if tokens <= 0 {
		t.Errorf("expected positive token count, got %d", tokens)
	}
	// "hello world" = 11 chars -> (11+3)/4 + 1 = 4
	// "hi there" = 8 chars -> (8+3)/4 + 1 = 3
	// "test" = 4 chars -> (4+3)/4 + 1 = 2
	// Each message has 20 overhead -> 60
	// Total ~ 4 + 3 + 2 + 60 = 69
	if tokens < 30 || tokens > 200 {
		t.Errorf("token estimate %d seems unreasonable (expected ~69)", tokens)
	}
}

func TestNeedsCompaction(t *testing.T) {
	conv := NewConversation("x")
	// Add many messages to push past threshold
	for i := 0; i < 50; i++ {
		conv.AddUser("hello world this is a test message with enough text to generate tokens")
		conv.AddAssistant("response that is also long enough to matter for token estimation purposes", "", nil)
	}
	// With context_limit=1000, threshold=50% -> 500 tokens
	needs := conv.NeedsCompaction(1000, 50)
	if !needs {
		t.Error("expected compaction to be needed for 50 long messages with small context limit")
	}

	// With very large context limit, should not need compaction
	needs = conv.NeedsCompaction(1000000, 90)
	if needs {
		t.Error("expected no compaction needed with huge context limit")
	}
}

func TestCompactRemovesDroppable(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hi")

	// Assistant with tool call
	tc := ToolCall{Index: 0, ID: "call_1", Name: "tool_a", Arguments: "{}"}
	conv.AddAssistant("", "", []ToolCall{tc})
	conv.AddTool("call_1", "result")

	// Final answer
	conv.AddAssistant("done", "", nil)

	// Before compaction, should have 5 messages (system + user + assistant(tool) + tool + assistant(content))
	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 5 {
		t.Fatalf("before compact: expected 5 msgs, got %d", len(msgs))
	}

	conv.Compact()

	// After compaction, the tool result should be removed (Droppable) but
	// orphaned tool-call assistant should also be removed.
	msgs = conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("after compact: expected 3 msgs (system + user + final), got %d", len(msgs))
	}
}

func TestCompactRemovesOrphanedToolCalls(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hi")

	// Assistant with tool call
	tc := ToolCall{Index: 0, ID: "call_orphan", Name: "tool_a", Arguments: "{}"}
	conv.AddAssistant("", "", []ToolCall{tc})

	// Remove the tool result by compacting before adding it
	conv.Compact()

	msgs := conv.ToOpenAIMessages()
	// After compact: orphaned tool-call assistant should be gone, only system + user remain
	if len(msgs) != 2 {
		t.Fatalf("expected 2 msgs after compact (system + user), got %d", len(msgs))
	}
}

func TestCompactWithSummaryCallback(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("What is Go?")
	conv.AddAssistant("Go is a programming language.", "", nil)

	summaryCalled := false
	conv.SetSummaryCallback(func(messages []Message, maxTokens int) *string {
		summaryCalled = true
		s := "Summary: discussed Go language"
		return &s
	})

	conv.Compact()

	if !summaryCalled {
		t.Error("SummaryCallback should have been called")
	}

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 2 {
		t.Fatalf("expected 2 msgs after compact with summary (system + summary), got %d", len(msgs))
	}
	if msgs[1]["role"] != "user" {
		t.Errorf("summary message role = %v, want 'user'", msgs[1]["role"])
	}
	content, ok := msgs[1]["content"].(string)
	if !ok || !strings.Contains(content, "Summary:") {
		t.Errorf("summary content = %v, should contain 'Summary:'", msgs[1]["content"])
	}
}

func TestCompactNoSummaryCallback(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("Hello")
	conv.AddAssistant("World", "", nil)

	// Without a summary callback, compact should be a no-op (no droppable messages)
	conv.Compact()

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 msgs after compact without summary, got %d", len(msgs))
	}
}

func TestMarkSupersededToolCalls(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("do something")

	// First tool round
	tc1 := ToolCall{Index: 0, ID: "call_1", Name: "tool_a", Arguments: "{}"}
	conv.AddAssistant("", "thinking...", []ToolCall{tc1})
	conv.AddTool("call_1", "result1")

	// Second tool round
	tc2 := ToolCall{Index: 0, ID: "call_2", Name: "tool_b", Arguments: "{}"}
	conv.AddAssistant("", "more thinking...", []ToolCall{tc2})
	conv.AddTool("call_2", "result2")

	// Final answer
	conv.AddAssistant("final answer", "", nil)

	// After compaction, both tool rounds should be removed
	conv.Compact()

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 msgs (system + user + final), got %d", len(msgs))
	}
	if msgs[2]["content"] != "final answer" {
		t.Errorf("final msg content = %v, want 'final answer'", msgs[2]["content"])
	}
}

func TestTruncate(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("msg1")
	conv.AddAssistant("resp1", "", nil)
	conv.AddUser("msg2")
	conv.AddAssistant("resp2", "", nil)

	if conv.Size() != 4 {
		t.Fatalf("size before truncate = %d, want 4", conv.Size())
	}

	conv.Truncate(2)
	if conv.Size() != 2 {
		t.Fatalf("size after truncate = %d, want 2", conv.Size())
	}
}

func TestMultipleToolCallsSuperseded(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("do research")

	// Three tool rounds
	for i := 0; i < 3; i++ {
		id := "call_" + string(rune('a'+i))
		tc := ToolCall{Index: 0, ID: id, Name: "tool", Arguments: "{}"}
		conv.AddAssistant("", "thinking...", []ToolCall{tc})
		conv.AddTool(id, "data")
	}

	// Final answer
	conv.AddAssistant("Here is the result", "", nil)

	conv.Compact()

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 msgs after compact (system + user + final), got %d", len(msgs))
	}
}

func TestEmptyConversation(t *testing.T) {
	conv := NewConversation("test")

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 1 {
		t.Fatalf("expected 1 message (system only), got %d", len(msgs))
	}
	if msgs[0]["role"] != "system" {
		t.Errorf("role = %v", msgs[0]["role"])
	}
}

func TestSetSystemPrompt(t *testing.T) {
	conv := NewConversation("old prompt")
	conv.SetSystemPrompt("new prompt")

	if conv.SystemPrompt() != "new prompt" {
		t.Errorf("SystemPrompt = %q, want %q", conv.SystemPrompt(), "new prompt")
	}

	msgs := conv.ToOpenAIMessages()
	if msgs[0]["content"] != "new prompt" {
		t.Errorf("system content = %v, want 'new prompt'", msgs[0]["content"])
	}
}

func TestAddToolWithoutToolCallsBefore(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hi")

	// Adding a tool result without a preceding tool-call assistant message
	// This shouldn't panic; the tool result is added but may be orphaned.
	conv.AddTool("call_orphan", "result")

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 msgs (system + user + tool), got %d", len(msgs))
	}
	if msgs[2]["role"] != "tool" {
		t.Errorf("msg[2] role = %v, want 'tool'", msgs[2]["role"])
	}
}

func TestLongConversationTokenEstimate(t *testing.T) {
	conv := NewConversation("x")
	// Add one very long message
	longText := strings.Repeat("hello world ", 1000)
	conv.AddUser(longText)
	conv.AddAssistant("ok", "", nil)

	tokens := conv.EstimateTotalTokens()
	if tokens <= 0 {
		t.Errorf("expected positive token count, got %d", tokens)
	}
	// ~11000 chars / 4 ≈ 2750 tokens + overhead
	if tokens < 1000 || tokens > 10000 {
		t.Errorf("token estimate %d seems unreasonable for ~11K chars", tokens)
	}
}

func TestConversationUserAndContentRetainPreserve(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("user msg")
	conv.AddAssistant("assistant msg", "", nil)

	// Internally, check that user and content-bearing assistant messages are retained
	// Compact shouldn't remove them
	conv.Compact()
	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 3 {
		t.Fatalf("expected 3 msgs after compact, got %d", len(msgs))
	}
}

func TestConversationCompactPreservesStreamOrder(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("q1")
	conv.AddAssistant("a1", "", nil)
	conv.AddUser("q2")
	conv.AddAssistant("a2", "", nil)

	conv.Compact()

	msgs := conv.ToOpenAIMessages()
	if len(msgs) != 5 {
		t.Fatalf("expected 5 msgs (system + user1 + asst1 + user2 + asst2), got %d", len(msgs))
	}
}

func TestNilContentMarshal(t *testing.T) {
	conv := NewConversation("test")
	conv.AddUser("hi")

	tc := ToolCall{Index: 0, ID: "c1", Name: "t", Arguments: "{}"}
	conv.AddAssistant("", "", []ToolCall{tc})

	// This should marshal to JSON without issues
	msgs := conv.ToOpenAIMessages()
	data, err := json.Marshal(msgs)
	if err != nil {
		t.Fatalf("json.Marshal: %v", err)
	}
	if !strings.Contains(string(data), "null") {
		t.Logf("tool-call message content should be null in JSON: %s", data)
	}
}
