# Phase 1: Core Types, Config, PlanBoard

## Goal

Implement the foundational data types, environment-based configuration loading, and the PlanBoard document store — all with tests first.

## Files

| File | Purpose |
|------|---------|
| `config/config.go` | `Config` struct + `FromEnv()` |
| `config/config_test.go` | Tests for env loading |
| `chat/types.go` | `Message`, `ToolCall`, `Usage`, `DisplayEntry`, `EntryType`, `ToolAccumulator` |
| `chat/types_test.go` | Tests for types + accumulator |
| `plan/planboard.go` | `PlanBoard` struct |
| `plan/planboard_test.go` | Tests for plan operations |

---

## Step 1.1: config/config.go

### Types

```go
package config

type Config struct {
    APIBase           string
    APIKey            string
    Model             string
    ReasoningEffort   string
    SafeDir           string
    SearchAPIKey      string
    SearchEngineID    string
    SearchEndpoint    string
    WorktreeBase      string
    ReadOnlyPaths     []string
    MaxToolIterations int
    ContextLimit      int
    CompactThreshold  int
    SystemPrompt      string
}

func FromEnv() Config { ... }
```

### Failing Tests: `config/config_test.go`

1. **TestDefaults** — `FromEnv()` with no env vars set returns zero-value / default fields
2. **TestAPIBaseEnv** — `LLM_API` overrides default, `API_BASE` is fallback
3. **TestAPIKeyEnv** — `LLM_KEY` / `API_KEY` env var
4. **TestModelEnv** — `MODEL` env var
5. **TestSafeDirEnv** — `SAFE_DIR` is used; when empty, defaults to `os.Getwd()`
6. **TestMaxToolIterations** — `LLM_MAX_TOOL_ITERATIONS` parsing and clamping
7. **TestContextLimit** — `LLM_CONTEXT_LIMIT` parsing
8. **TestCompactThreshold** — `LLM_COMPACT_THRESHOLD` parsing, clamping to 0-100
9. **TestReadOnlyPaths** — `READ_ONLY_PATHS` colon-separated, with canonicalization
10. **TestSearchParams** — `SEARCH_API_KEY`, `SEARCH_ENGINE_ID`, `SEARCH_ENDPOINT`
11. **TestWorktreeBase** — `WORKTREE_BASE` env var
12. **TestSystemPrompt** — `LLM_SYSTEM_PROMPT` / `SYSTEM_PROMPT` override
13. **TestReasoningEffort** — `LLM_REASONING_EFFORT` env var

---

## Step 1.2: chat/types.go

### Types

```go
package chat

type Usage struct {
    PromptTokens     int `json:"prompt_tokens"`
    CompletionTokens int `json:"completion_tokens"`
    TotalTokens      int `json:"total_tokens"`
}

type ToolCall struct {
    Index     int    `json:"index"`
    ID        string `json:"id"`
    Name      string `json:"name"`
    Arguments string `json:"arguments"`
}

type Message struct {
    Role             string      `json:"role"`
    Content          *string     `json:"content"` // nil for tool_call-only messages
    ReasoningContent string      `json:"reasoning_content,omitempty"`
    ToolCalls        []ToolCall  `json:"tool_calls,omitempty"`
    ToolCallID       string      `json:"tool_call_id,omitempty"`
    Retain           bool        `json:"-"` // true = Preserve, false = Droppable
}

type EntryType int
const (
    EntryUserText  EntryType = iota
    EntryReasoning
    EntryContent
    EntryToolCall
)

type DisplayEntry struct {
    Type        EntryType
    Text        string
    IsStreaming bool
    Seq         int
}

// ToolAccumulator merges streaming tool_call deltas.
type ToolAccumulator struct { ... }
func (a *ToolAccumulator) Apply(delta map[string]any)
func (a *ToolAccumulator) HasCalls() bool
func (a *ToolAccumulator) Finalize() []ToolCall
```

### Failing Tests: `chat/types_test.go`

1. **TestToolAccumulatorSingleChunk** — all fields in one chunk
2. **TestToolAccumulatorMultiChunk** — arguments concatenated across chunks
3. **TestToolAccumulatorNoToolCalls** — delta with no tool_calls
4. **TestToolAccumulatorEmptyDelta** — empty object
5. **TestToolAccumulatorMultipleParallel** — two tool calls interleaved
6. **TestToolAccumulatorFinalizeOrder** — calls sorted by index
7. **TestUsageFromJSON** — deserialize from JSON map
8. **TestDisplayEntry** — basic struct creation
9. **TestSanitizeUTF8** — valid UTF-8 passes through, invalid replaced with U+FFFD (if implemented)

---

## Step 1.3: plan/planboard.go

### Types

```go
package plan

type PlanBoard struct { ... }

func New() *PlanBoard
func (b *PlanBoard) WritePlan(markdown string) error
func (b *PlanBoard) ReadPlan() (string, error)
func (b *PlanBoard) CommentPlan(comment string) error
```

ReadPlan output format:
```
# Plan

{markdown body}

---

## Comments

### Comment 1

{comment}

### Comment 2

{comment}
```

If plan body is empty: `"(empty plan)"`
If no comments: omit the comments section.

### Failing Tests: `plan/planboard_test.go`

1. **TestEmptyPlan** — `ReadPlan()` on fresh board returns "(empty plan)"
2. **TestWriteAndRead** — write plan, read it back
3. **TestWriteOverwritesBody** — second write replaces body, comments cleared
4. **TestCommentAppends** — `CommentPlan` appends, read shows both
5. **TestCommentPreservedAfterWrite** — write then comment, second write clears comments (comments are NOT preserved across writes)
6. **TestEmptyCommentError** — `CommentPlan("")` returns error
7. **TestMultipleComments** — several comments, all appear in read
8. **TestNewlineHandling** — plan with multiple lines
9. **TestReadPlanFormat** — exact output format matches spec

---

## Running Phase 1 Tests

```bash
cd go
go mod init cima
go test ./config/... ./chat/... ./plan/...
```
