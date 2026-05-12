# Phase 3: Conversation History

## Goal

Implement the `Conversation` type that manages the message history, serializes to OpenAI-compatible JSON, estimates token counts, and supports compaction.

## Files

| File | Purpose |
|------|---------|
| `chat/conversation.go` | `Conversation` — message history management |
| `chat/conversation_test.go` | Tests for conversation operations |

---

## Step 3.1: chat/conversation.go

### Type

```go
package chat

type Conversation struct { ... }

// SummaryCallback produces a summary of a set of messages within a token budget.
type SummaryCallback func(messages []Message, maxTokens int) *string

func NewConversation(systemPrompt string) *Conversation
func (c *Conversation) AddUser(content string)
func (c *Conversation) AddAssistant(content, reasoning string, toolCalls []ToolCall)
func (c *Conversation) AddTool(toolCallID, content string)
func (c *Conversation) SetSystemPrompt(prompt string)
func (c *Conversation) Size() int
func (c *Conversation) Truncate(n int)
func (c *Conversation) Clear()
func (c *Conversation) ToOpenAIMessages() []map[string]any
func (c *Conversation) SystemPrompt() string
func (c *Conversation) NeedsCompaction(contextLimit int, thresholdPct int) bool
func (c *Conversation) Compact()
func (c *Conversation) SetSummaryCallback(cb SummaryCallback)
func (c *Conversation) EstimateTotalTokens() int
```

### Behaviour

- **`AddUser`**: appends a user message with `Preserve` retention
- **`AddAssistant`**:
  - If `toolCalls` is non-empty: content is `nil`, retains tool_calls. Otherwise content is set.
  - Before adding a content-bearing assistant message, marks previous tool-call rounds as `Droppable`.
  - Sets reasoning_content if provided.
- **`AddTool`**: appends a tool result message with `Droppable` retention
- **`ToOpenAIMessages`**: returns `[]map[string]any` in OpenAI format with system prompt first:
  ```json
  [{"role": "system", "content": "..."},
   {"role": "user", "content": "..."},
   {"role": "assistant", "content": "...", "reasoning_content": "..."},
   {"role": "assistant", "content": null, "tool_calls": [...]},
   {"role": "tool", "tool_call_id": "...", "content": "..."}]
  ```
- **`EstimateTotalTokens`**: fast approximation: `(len(text) + 3) / 4 + 1` per string field + 20 overhead per message
- **`NeedsCompaction`**: returns true if `EstimateTotalTokens() > contextLimit * thresholdPct / 100`
- **`Compact`**:
  1. Remove all `Droppable` messages
  2. Remove orphaned assistant tool-call messages (no matching tool result)
  3. If `SummaryCallback` is set, call it with remaining messages → replace history with a single summary message
- **`Clear`**: clears messages, preserves system prompt
- **`Truncate`**: truncates to n messages (for error recovery)
- Token estimation adds reasoning_content, tool_calls (id, name, arguments), tool_call_id to message total

---

### Failing Tests: `chat/conversation_test.go`

1. **TestBasicExchange** — user + assistant, 3 messages including system
2. **TestWithReasoning** — assistant has reasoning_content
3. **TestWithToolCalls** — assistant has tool_calls, content is null
4. **TestWithToolResult** — full cycle: user → assistant(tool_calls) → tool → assistant(content)
5. **TestClearPreservesSystem** — Clear doesn't remove system prompt
6. **TestToOpenAIMessagesFormat** — exact JSON structure matches spec
7. **TestReasoningContentOmittedWhenEmpty** — empty reasoning not in output
8. **TestEstimateTokens** — approximate token counting
9. **TestNeedsCompaction** — threshold logic
10. **TestCompactRemovesDroppable** — tool results marked Droppable are removed
11. **TestCompactRemovesOrphanedToolCalls** — assistant tool-call without matching tool result removed
12. **TestCompactWithSummaryCallback** — callback invoked, messages replaced with summary
13. **TestCompactNoSummaryCallback** — only droppable removal happens
14. **TestMarkSupersededToolCalls** — after content-bearing assistant, previous tool rounds become droppable
15. **TestTruncate** — size reduced correctly
16. **TestMultipleToolCallsSuperseded** — several tool rounds before final answer
17. **TestEmptyConversation** — only system prompt
18. **TestSetSystemPrompt** — system prompt can be changed after creation
19. **TestAddToolWithoutToolCallsBefore** — standalone tool result (edge case)
20. **TestLongConversationTokenEstimate** — large messages, estimate is reasonable

---

## Running Phase 3 Tests

```bash
cd go
go test ./chat/...
```
