# Phase 2: HTTP Client + SSE Parser

## Goal

Implement the SSE streaming parser and the OpenAI-compatible HTTP client that uses it. All HTTP interactions are testable via `net/http/httptest`.

## Files

| File | Purpose |
|------|---------|
| `client/sse.go` | `SSEParser` — incremental SSE line parser |
| `client/sse_test.go` | Tests for SSE parsing |
| `client/client.go` | `Client` — OpenAI-compatible chat/completions HTTP client |
| `client/client_test.go` | Tests for HTTP client |

---

## Step 2.1: client/sse.go

### Type

```go
package client

type SSEParser struct { ... }

type SSECallbacks struct {
    OnData func(data map[string]any)  // called for each parsed JSON data line
    OnDone func()                     // called for [DONE]
    OnError func(err string)          // called for JSON parse errors
}

func NewSSEParser(cb SSECallbacks) *SSEParser
func (p *SSEParser) Feed(data string)      // feed raw chunk from HTTP body
func (p *SSEParser) Flush()                // process any remaining buffered data
func (p *SSEParser) Reset()                // clear buffer and raw state
func (p *SSEParser) Raw() string           // accumulated raw SSE text
```

### Behaviour

- Lines split on `\n`
- Lines starting with `data: ` have that prefix stripped and are accumulated
- Empty lines are ignored (SSE event separators are the `\n\n` delimiter; Feed splits on `\n`, so empty string = the blank line between events)
- `[DONE]` calls `OnDone`
- JSON parsing errors call `OnError`
- Non-data lines (`event:`, `:keepalive`, etc.) are ignored
- `Flush` processes any buffered incomplete data as a complete line
- `Reset` clears buffer and raw state but keeps callbacks
- `Raw` returns all raw bytes fed since creation / last reset

### Failing Tests: `client/sse_test.go`

1. **TestParseSingleCompleteEvent** — one `data: {...}\n\n` triggers `OnData`
2. **TestParseMultipleEvents** — two events + `[DONE]` in one Feed call
3. **TestParsePartialAcrossFeeds** — first Feed has `data: {"key`, second has `":1}\n\n`
4. **TestParseIgnoresNonDataLines** — `event: test\n` before data line
5. **TestParseDoneSignal** — `data: [DONE]\n\n` triggers `OnDone`
6. **TestParseMalformedJSON** — `data: {invalid}\n\n` triggers `OnError`
7. **TestFlushBufferedData** — incomplete line at end, `Flush()` processes it
8. **TestFlushWithNoBuffered** — flush on empty buffer is a no-op
9. **TestResetClearsBuffer** — feed partial, reset, feed complete — only complete received
10. **TestRawAccumulation** — Raw() returns all text fed
11. **TestFeedWithCarriageReturn** — lines ending with `\r\n` are handled (strip `\r`)
12. **TestEmptyFeed** — empty string does nothing
13. **TestVeryLongLine** — buffer grows correctly with long data

---

## Step 2.2: client/client.go

### Type

```go
package client

type Client struct { ... }

func New(apiBase, apiKey string) *Client
func (c *Client) URL() string
func (c *Client) ModelsURL() string

// Chat sends a non-streaming request and returns the full JSON response.
func (c *Client) Chat(ctx context.Context, payload map[string]any) (map[string]any, error)

// StreamChat sends a streaming request, calling SSEParser callbacks as data arrives.
func (c *Client) StreamChat(ctx context.Context, payload map[string]any, cb SSECallbacks) error

// FetchModels queries /v1/models and returns the list of model IDs.
func (c *Client) FetchModels(ctx context.Context) ([]string, error)

// FetchModelContextLimit tries to discover the context window for a model.
func (c *Client) FetchModelContextLimit(ctx context.Context, model string) (int, error)

// LastRawResponse returns the raw body of the last response (for debugging).
func (c *Client) LastRawResponse() string
```

### Behaviour

- **`Chat`**: POST to `/chat/completions` with JSON payload → parse full response
- **`StreamChat`**: POST to `/chat/completions` with `"stream": true` → incremental SSE parsing
- **`FetchModels`**: GET `/v1/models` → parse JSON array
- **`FetchModelContextLimit`**: calls FetchModels, looks for context window fields in model metadata
- **Retry logic**: exponential backoff with jitter for 429/5xx, max 3 retries
- **Headers**: `Content-Type: application/json`, `Authorization: Bearer {key}` if key non-empty
- **SSL**: verify peers, system CA bundle
- **Cancellation**: via `context.Context`
- **User-Agent**: `cima/0.1`
- **Timeouts**: connect 10s, overall 3600s for stream, 30s for queries
- **Auto-decode**: `Accept-Encoding: gzip` (Go's `net/http` does this by default)

### Failing Tests: `client/client_test.go`

Uses `net/http/httptest.NewServer` to mock the API.

1. **TestChatBasic** — mock returns valid JSON, `Chat` returns parsed map
2. **TestChatHTTPError** — mock returns 500, `Chat` returns error with HTTP code
3. **TestChatJSONError** — mock returns invalid JSON, `Chat` returns parse error
4. **TestChatAuth** — verify Authorization header sent when key is set
5. **TestChatAuthEmpty** — no Authorization header when key is empty
6. **TestStreamChatBasic** — mock streams SSE events, callbacks fire
7. **TestStreamChatDone** — mock sends `[DONE]`, `OnDone` fires
8. **TestStreamChatError** — mock returns 500 for stream, error returned
9. **TestStreamChatCancellation** — cancel context mid-stream, StreamChat returns
10. **TestFetchModels** — mock returns model list, parsed correctly
11. **TestFetchModelsError** — mock returns 500, error returned
12. **TestFetchContextLimit** — mock returns model with `max_model_len`, returns that value
13. **TestFetchContextLimitNotFound** — mock returns model without context fields, returns 0
14. **TestFetchContextLimitMultipleFields** — test priority order of field names
15. **TestRetryOn429** — mock returns 429 twice then 200, succeeds on third attempt
16. **TestRetryOn500** — mock returns 500 then 200, succeeds
17. **TestRetryExhausted** — mock always returns 429, returns error after max retries
18. **TestLastRawResponse** — verify raw body is captured
19. **TestMakeHeaders** — Content-Type always present, Authorization conditional
20. **TestURLConstruction** — trailing slash stripped from base URL

---

## Running Phase 2 Tests

```bash
cd go
go test ./client/...
```
