# Phase 5: Chat Session Orchestrator

## Goal

Implement `ChatSession` — the central orchestration loop that ties together the client, conversation, tools, and plan board. This is the "brain" that runs the conversation-turn → tool-execution → conversation-turn loop until the LLM produces a final answer.

## Files

| File | Purpose |
|------|---------|
| `chat/session.go` | `ChatSession` — orchestrates one user turn |
| `chat/session_test.go` | Tests with httptest mock API |

---

## Step 5.1: chat/session.go

### Type

```go
package chat

type ChatResult struct {
    Content   string
    Reasoning string
}

type OutputCallback func(text string, entryType EntryType)

type ChatSession struct { ... }

func NewSession(cfg config.Config, planBoard *plan.PlanBoard) *ChatSession

// RunOnce executes a single user turn: send message, process tool calls (iterative),
// return final assistant response or error.
func (s *ChatSession) RunOnce(ctx context.Context, userInput string) (*ChatResult, error)

func (s *ChatSession) SetModel(model string)
func (s *ChatSession) Model() string
func (s *ChatSession) Clear()
func (s *ChatSession) Compact()
func (s *ChatSession) PlanBoard() *plan.PlanBoard
func (s *ChatSession) SetOutputCallback(cb OutputCallback)
func (s *ChatSession) LastUsage() Usage
func (s *ChatSession) SafeDir() string
func (s *ChatSession) ClientForModels() *client.Client
```

### RunOnce Algorithm

```
1. Discover context limit (once per model/endpoint; cached)
2. Check conversation.needsCompaction → compact if needed
3. AddUser(userInput)
4. LOOP (max maxIterations):
   a. Build payload: model, reasoning_effort, messages (from conversation.toOpenAIMessages()),
      tools (from registry.toOpenAITools()), stream: true
   b. Call client.StreamChat with SSE callbacks:
      - OnData: accumulate reasoning_content, content, tool_calls (via ToolAccumulator), usage
      - OnDone: set done flag
      - OnError: set error flag
   c. If stream error and no content → return error (truncate conversation)
   d. toolCalls = accumulator.finalize()
   e. If toolCalls empty:
      - conversation.addAssistant(content, reasoning)
      - return ChatResult{content, reasoning}
   f. Else:
      - conversation.addAssistant("", reasoning, toolCalls)
      - For each tool call (serial if any write tool, parallel if all read-only):
        i.   Check cancellation
        ii.  OutputCallback tool invocation
        iii. Execute tool → result/error
        iv.  conversation.addTool(id, result/error)
      - Continue loop
5. If max iterations reached → return error "Maximum tool call iterations reached"
```

### Behaviour

- **Context limit discovery**: one-time per `{client.URL}:{model}` key, cached in a `sync.Map`
- **Conversation compaction**: triggers when `NeedsCompaction` returns true (before building payload)
- **Parallel execution**: read-only tools execute concurrently via goroutines
- **Serial execution**: if any tool in the batch has Write permission, execute sequentially to avoid git lock conflicts
- **Cancellation**: checked before each tool execution; sets cancelled flag in `context.Context`
- **Error recovery**: on stream error, truncate conversation to pre-RunOnce snapshot
- **Output callbacks**: called during streaming (reasoning, content, tool invocations) for UI updates
- **Summary callback**: wired to `RunOnce`'s compaction by calling LLM to summarize old messages

### Failing Tests: `chat/session_test.go`

Uses `httptest.NewServer` to mock the OpenAI-compatible API.

1. **TestSimpleExchange** — user message → LLM responds with content → session returns content
2. **TestWithReasoning** — LLM sends reasoning_content → session captures it
3. **TestSingleToolCall** — LLM calls one tool, tool succeeds, LLM responds with final content
4. **TestMultipleToolCalls** — LLM calls several tools, all succeed, final answer
5. **TestToolCallError** — tool returns error, LLM sees error in conversation
6. **TestCancellationDuringStream** — cancel context mid-stream → error "Interrupted"
7. **TestCancellationDuringTool** — cancel context during tool execution → error
8. **TestMaxIterations** — LLM keeps calling tools → error after limit
9. **TestContextLimitDiscovery** — first call fetches model metadata
10. **TestContextLimitCached** — second call uses cached value
11. **TestConversationCompaction** — long conversation triggers compaction before API call
12. **TestOutputCallbackStreamingContent** — callback receives incremental content
13. **TestOutputCallbackReasoning** — callback receives reasoning
14. **TestOutputCallbackToolInvocation** — callback receives "→ tool(args)" for each tool
15. **TestSerialToolExecution** — write tools execute sequentially
16. **TestParallelToolExecution** — read-only tools execute in parallel
17. **TestStreamErrorRecovery** — stream error with no content → conversation truncated
18. **TestStreamErrorPartialContent** — stream error with some content → that content returned
19. **TestModelChange** — SetModel changes model in payload
20. **TestClearResetsConversation** — Clear empties history (except system prompt)

---

## Running Phase 5 Tests

```bash
cd go
go test ./chat/...
```
