# Code Review: llm-chat — Context Window Usage & Tooling Investigation

> **Date:** 2025-01-XX  
> **Scope:** Full codebase review with focus on context-window efficiency, request-handling layer, and tooling gaps for agentic coding.

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Architecture Overview](#2-architecture-overview)
3. [Investigation: Context Window Usage](#3-investigation-context-window-usage)
4. [Investigation: Request Handling Layer](#4-investigation-request-handling-layer)
5. [Investigation: Tooling Gaps for Agentic Coding](#5-investigation-tooling-gaps-for-agentic-coding)
6. [Priority Recommendations](#6-priority-recommendations)
7. [Appendix: Tool Inventory](#7-appendix-tool-inventory)

---

## 1. Executive Summary

**Primary finding:** The heavy context window usage is caused by **unbounded conversation growth** — every message, tool call, and tool result is appended to the conversation history indefinitely and the entire history is sent with every API request. There is zero trimming, summarization, or token-aware management.

**Secondary finding:** The app lacks several critical tools that would let the model accomplish tasks with **fewer turns and smaller payloads** — in particular `file_diff`/`apply_patch`, `project_tree`, and `git` integration. Missing these forces the model to make many more tool calls than necessary, each adding large results back into the conversation.

**Tertiary finding:** The request-handling layer has minor inefficiencies (uncached tool schemas, no gzip, fragile retry logic) but these are not the primary driver of context-window bloat.

---

## 2. Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                    main.cpp                      │
│         Config::from_env() → gui_main()          │
└─────────────────────┬───────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│                gui_app.cpp                       │
│     SDL3 + ImGui event loop, font loading        │
│     Owns ChatSession + AsyncChatState             │
└─────────────────────┬───────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│               gui_chat.cpp                       │
│     Chat UI rendering, streaming entry display   │
│     start_chat / cancel_chat / drain_pending     │
└─────────────────────┬───────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│                chat.cpp / chat.h                  │
│     ChatSession: owns Conversation, ChatClient,   │
│     ToolRegistry. run_once() is the main loop.    │
│                                                   │
│     ┌──────────┐    ┌────────────┐               │
│     │Conversa- │    │ChatClient  │               │
│     │tion      │───▶│(libcurl)   │               │
│     │(messages)│    │stream_chat │               │
│     └──────────┘    │→SSEParser  │               │
│                     └────────────┘               │
│     ┌──────────────┐                             │
│     │ToolRegistry  │                             │
│     │(7 tools)     │                             │
│     └──────────────┘                             │
└─────────────────────────────────────────────────┘
```

**Data flow for each `run_once` call:**

1. Add user message → `conversation_.add_user()`
2. Build payload: `conversation_.to_openai_messages()` + `tools_.to_openai_tools()`
3. Stream response via `client_.stream_chat()`
4. If tool calls received → execute tool(s) → add tool results → **loop back to step 2**
5. If content received → add assistant message → return

---

## 3. Investigation: Context Window Usage

### 3.1 Root Cause: No Context Management

**File:** `chat.cpp` lines 32–38  
**Problem:** Every iteration of the tool-call loop sends **the entire conversation history**:

```cpp
json payload = {{"model", model_},
    {"messages", conversation_.to_openai_messages()},  // ← ALL messages
    {"tools", tools_.to_openai_tools()},                // ← ALL tool schemas
    {"stream", true}};
```

**File:** `types.cpp` (Conversation class)  
**Problem:** Messages accumulate indefinitely. The `truncate()` method exists but is only used for **error rollback** — never for context-window management.

| Call site | File:Line | Purpose |
|-----------|-----------|---------|
| `chat.cpp:79` | Rollback on stream error | ← only usage |
| `chat.cpp:87` | Rollback on streaming error with no content | ← only usage |
| `chat.cpp:97` | Rollback on interrupt | ← only usage |
| `chat.cpp:107` | Rollback on interrupt (multi-call) | ← only usage |
| `chat.cpp:118` | Rollback on interrupt (single call) | ← only usage |
| `chat.cpp:125` | Rollback on max iterations | ← only usage |

**No function** exists to trim old messages, summarize them, or apply a sliding window.

### 3.2 Amplifying Factor: Max Tool Iterations

**File:** `chat.h` line 31, `config.h` line 22

| Setting | Value |
|---------|-------|
| Hardcoded default in `ChatSession` | `max_iterations_ = 100` |
| Config default (env `LLM_MAX_TOOL_ITERATIONS`) | `50` |
| Effective default (overridden by config) | `50` |

Typical agentic coding tasks need **3–10 iterations**. The high default means a runaway tool-calling loop can generate **50+ rounds** of assistant messages + tool results before the limit kicks in. Each round adds:
- The assistant's tool-call message (reasoning content + tool call JSON)
- The tool result(s) (potentially very large)

### 3.3 Amplifying Factor: Large Tool Results

Every tool result is stored verbatim in the conversation and re-sent on every subsequent request.

| Tool | Maximum output size | Realistic worst-case contribution per call |
|------|--------------------|-------------------------------------------|
| `run_bash` | 16,000 chars / 500 lines | **~4,000 tokens** (if output is large) |
| `read_file` | 400 lines | **~2,000–4,000 tokens** (for a 400-line file) |
| `grep_files` | 200 matches | **~2,000+ tokens** (200 lines of context) |
| `list_files` | unbounded (directory listing) | **~200–1,000 tokens** (large directory) |
| `edit_file` | confirmation string | **~50 tokens** |
| `write_file` | confirmation string | **~50 tokens** |
| `web_search` | 10 results | **~1,000–2,000 tokens** |

A single iteration with two tool calls (e.g., `grep_files` + `read_file`) can add **4,000–8,000 tokens** to the conversation. After 10 such iterations, that's **40k–80k tokens** just from tool results, plus all the messages themselves.

### 3.4 Missing: Token Counting

**File:** `types.h` lines 11–16, `chat.h` line 40

The `Usage` struct is populated from the API response but **never used to drive any decision**:

```cpp
struct Usage {
    int prompt_tokens = 0;      // ← populated but unused
    int completion_tokens = 0;  // ← populated but unused
    int total_tokens = 0;       // ← populated but unused
};
```

The `last_usage_` field in `ChatSession` is only displayed in the UI status bar (`gui_chat.cpp:345-349`). It is never:
- Compared to the model's context limit
- Used to trigger truncation
- Logged or accumulated over the session

### 3.5 Missing: Conversation Summarization

There is no mechanism to summarize old messages when the context window is full. A sliding window approach would work but requires:
1. Token counting (see 3.4)
2. A summarization step (call the LLM with a condensed prompt)
3. Replacement of old messages with the summary

None of these exist.

---

## 4. Investigation: Request Handling Layer

### 4.1 Uncached Tool Schemas

**File:** `chat.cpp` line 36, `tools.cpp` lines 565–575

`tools_.to_openai_tools()` serializes **all 7 tool definitions** to JSON on every API request. The tools don't change between calls (except when the mode toggles, which affects which tools are advertised). This is wasted work and adds a small but unnecessary overhead to each request payload.

**Impact:** Minor. The 7 tool schemas total maybe 2–4 KB uncompressed. But it's an easy fix.

### 4.2 No HTTP Compression

**File:** `client.cpp` (setup_curl function)

The curl handle does not set `Accept-Encoding: gzip`. Many LLM API providers support gzip-compressed streaming responses. For large conversations (50k+ tokens of history), compression can reduce bandwidth and memory usage by 5–10×.

**Impact:** Moderate. Adds unnecessary network transfer and memory pressure.

### 4.3 Fragile Retry Logic for Streaming

**File:** `client.cpp` lines 111–148 (stream_chat retry loop)

The retry mechanism uses a `data_delivered` flag to detect whether any SSE data was received before deciding whether to retry:

```cpp
bool data_delivered = false;
SSEParser::Callbacks guarded;
if (callbacks.on_data) {
    guarded.on_data = [&data_delivered, cb = std::move(callbacks.on_data)](const json& j) {
        data_delivered = true;
        cb(j);
    };
}
```

**Issues:**
- `on_done` also sets `data_delivered = true`, meaning if the server sends `[DONE]` but a network error occurs right after, the request won't be retried even though the response was incomplete.
- The `on_error` callback also sets the flag, so a transient error during chunked transfer prevents retry.
- Retry sleeps are in std::chrono::duration but don't check `g_interrupted` during the sleep.

**Impact:** Low under normal conditions. Could cause lost responses under flaky network conditions.

### 4.4 No Streaming Back-Pressure

**File:** `client.cpp` line 60 (write_stream callback)

The curl write callback feeds data directly to `SSEParser::feed()`, which parses and invokes `on_data` callbacks synchronously. If the UI thread is busy rendering, pending data accumulates in the SSEParser's `buf_` and `raw_` strings with no limit.

**Impact:** Low. In practice the UI thread is fast enough.

### 4.5 SSE Parser Is Strict

**File:** `types.cpp` lines 91–110

The parser only handles `data:` lines. Other SSE fields (`event:`, `id:`, `retry:`) are silently ignored. If the backend uses a non-standard SSE format (e.g., OpenAI's newer streaming format with `data:` containing JSON that includes both delta and usage), the parser handles it correctly because it doesn't check the event type. But there's no defense against malformed or unexpected SSE events.

**Impact:** Low. Works with all major OpenAI-compatible backends.

### 4.6 No Request Timeout on the Client Side

**File:** `client.cpp` line 54

The curl timeout is set to 3600 seconds (1 hour):

```cpp
curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);
```

This is effectively no timeout for normal chat usage. If the server hangs, the client will wait an hour. Combined with the max 50 tool iterations, a single `run_once` could theoretically hang for 50 hours.

**Impact:** Low for normal usage. Could be frustrating if the API is unresponsive.

---

## 5. Investigation: Tooling Gaps for Agentic Coding

### 5.1 Current Tool Inventory

| # | Tool | Mode Restrictions | Purpose |
|---|------|-------------------|---------|
| 1 | `list_files` | None | List files in a directory |
| 2 | `read_file` | None | Read lines from a file (max 400) |
| 3 | `grep_files` | None | Regex search (max 200 results) |
| 4 | `write_file` | Build only | Write content to a file |
| 5 | `edit_file` | Build only | Single search-and-replace edit |
| 6 | `run_bash` | Build only | Execute shell command (capped) |
| 7 | `web_search` | None | Search the web (Wikipedia/Google) |

### 5.2 Critical Gaps

These tools are **essential for effective agentic coding** and their absence forces the model to make many more tool calls, each adding large results to the conversation.

#### ❌ `file_diff` / `apply_patch`

**Why it matters:**  
The `edit_file` tool performs a single search-and-replace operation. For any multi-line change (e.g., adding a function, refactoring a class), the model must make **multiple `edit_file` calls** — one per hunk. Each call adds a tool result message to the conversation. A single `apply_patch` tool that accepts a unified diff could replace 3–10 `edit_file` calls.

**Impact on context window:**  
Replacing 5 `edit_file` calls with 1 `apply_patch` saves ~5 tool-result messages + 5 assistant tool-call messages = ~10 messages per code change.

**Implementation sketch:**
```cpp
Tool t;
t.name = "apply_patch";
t.description = "Apply a unified diff (patch) to a file. "
                "The patch must apply cleanly. Returns the result.";
t.parameters = {
    {"type", "object"},
    {"properties", {
        {"path", {{"type", "string"}, {"description", "File path to patch"}}},
        {"patch", {{"type", "string"}, {"description", "Unified diff content"}}}
    }},
    {"required", {"path", "patch"}}
};
t.execute = [safe_dir](const json& args) -> Result<std::string> {
    // Write patch to temp file, run `patch -p0 file patchfile`
};
```

#### ❌ `project_tree`

**Why it matters:**  
`list_files` only lists a single directory. To understand the project structure, the model has to call `list_files` recursively, potentially dozens of times. A single `project_tree` call could return the entire recursive file tree.

**Impact on context window:**  
Worst case: the model explores a deep directory tree with 20+ `list_files` calls. Each adds ~50–200 tokens of output. With `project_tree`, this becomes 1 call.

**Implementation sketch:**
```cpp
Tool t;
t.name = "project_tree";
t.description = "Get a recursive directory tree of the project. "
                "Returns a tree-like listing with indentation.";
t.parameters = {
    {"type", "object"},
    {"properties", {
        {"max_depth", {{"type", "integer"}, {"description", "Maximum depth (default 5)"}}}
    }},
    {"required", {}}
};
t.execute = [safe_dir](const json& args) -> Result<std::string> {
    int max_depth = args.value("max_depth", 5);
    // Walk directory recursively up to max_depth
};
```

#### ❌ `git` Integration

**Why it matters:**  
Agentic coding without git is painful. The model needs to check status, view diffs, commit changes, etc. Currently it must use `run_bash` for all git operations, which returns raw terminal output with ANSI codes, pager warnings, etc. A structured `git` tool would return clean, minimal data.

**Sub-tools needed:**
- `git_status` — return changed/untracked files
- `git_diff` — return unified diff of changes
- `git_log` — return recent commit history
- `git_commit` — stage and commit

**Impact on context window:**  
`run_bash git status` returns ~10–30 lines. `git_diff --staged` returns potentially hundreds of lines. With structured tools, the model gets exactly what it needs without the parsing overhead.

### 5.3 Important Gaps

#### `delete_file` / `move_file` / `rename_file`

**Why it matters:**  
The model can create and edit files but cannot delete or rename them. Must use `run_bash` with `rm`/`mv`, which adds raw output to the conversation.

#### `read_file_lines` (specific line ranges)

**Why it matters:**  
`read_file` reads from an offset for N lines. Models often want to read specific ranges like "lines 45–78" after a grep match at line 52. The current tool requires the model to compute the correct offset and max_lines.

**Better API:**
```json
{"path": "file.cpp", "start_line": 45, "end_line": 78}
```

#### `search_symbols` (CTags / LSP integration)

**Why it matters:**  
Regex search (`grep_files`) cannot find definitions, references, or declarations. A symbol search tool (powered by CTags, tree-sitter, or a language server) would let the model quickly navigate code without reading entire files.

#### `lint` / `format`

**Why it matters:**  
Running a linter or formatter is a common task. Currently requires `run_bash` with tool-specific invocation. A dedicated `lint` tool could return structured results (file:line:message).

#### `run_tests`

**Why it matters:**  
Running tests with structured output (pass/fail counts, test names). Currently requires `run_bash` and parsing raw output.

#### `web_fetch`

**Why it matters:**  
`web_search` returns search results but cannot fetch the content of a specific URL. Models often need to read documentation or API references.

### 5.4 Nice-to-Have Gaps

| Tool | Description |
|------|-------------|
| `terminal` (persistent shell) | Fire-and-forget `run_bash` loses state between calls. A persistent shell would let the model `cd`, set env vars, and build state incrementally. |
| `memory` / `scratchpad` | A file or key-value store where the model can persist notes, decisions, and context summaries across sessions. |
| `get_env` | Discover the runtime environment: OS, language versions, installed tools, environment variables. |
| `http_request` | Arbitrary HTTP requests (GET/POST) for API interaction beyond the chat backend. |
| `read_directory` | Structured directory listing with file sizes, permissions, and modification times (richer than `list_files`). |

---

## 6. Priority Recommendations

### P0 — Critical (Fix Now)

| # | Change | File(s) | Effort | Impact |
|---|--------|---------|--------|--------|
| 1 | **Add token counting + context trimming** — Count tokens in the conversation. When approaching the model's context limit (e.g., >70% of 128k), trim old messages (keep system prompt + last N messages + in-progress tool calls). | `types.h/cpp`, `chat.h/cpp` | 2–3 days | **High** — prevents context-window overflow |
| 2 | **Reduce default max_tool_iterations** — Change default from 50 to **15**. Gives enough headroom for complex tasks while bounding worst-case growth. | `config.h` | 5 min | **High** — bounds worst-case conversation size |
| 3 | **Cache tool schemas** — Cache `to_openai_tools()` JSON output, regenerate only on mode change. | `tools.h/cpp`, `chat.cpp` | 1 hr | **Medium** — reduces request payload size |

### P1 — Important (This Sprint)

| # | Change | File(s) | Effort | Impact |
|---|--------|---------|--------|--------|
| 4 | **Add `apply_patch` tool** — Accept unified diff input. Dramatically reduces the number of tool calls for code changes. | `tools.cpp` | 1 day | **High** — reduces tool-call count for edits |
| 5 | **Add `project_tree` tool** — Recursive directory listing in one call. | `tools.cpp` | 0.5 day | **High** — reduces tool-call count for exploration |
| 6 | **Add git tools** — `git_status`, `git_diff`, `git_log`, `git_commit`. | `tools.cpp` (new file: `git_tools.cpp`) | 2 days | **High** — essential for agentic coding |
| 7 | **Add Accept-Encoding: gzip** — Enable HTTP compression for streaming responses. | `client.cpp` | 15 min | **Medium** — reduces network/memory usage |

### P2 — Worthwhile (Next Sprint)

| # | Change | File(s) | Effort | Impact |
|---|--------|---------|--------|--------|
| 8 | **Conversation summarization** — When context is full, summarize old messages via LLM call and replace them with a condensed version. | `chat.h/cpp` | 2–3 days | **High** — preserves continuity while saving tokens |
| 9 | **Add `delete_file`, `move_file`, `rename_file`** — File management tools. | `tools.cpp` | 0.5 day | **Medium** — completes file I/O tool set |
| 10 | **Add `search_symbols` tool** — CTags or tree-sitter based symbol search. | `tools.cpp` (new file) | 3–5 days | **Medium** — enables precise code navigation |
| 11 | **Add `lint` / `format` tool** — Run linters/formatters with structured output. | `tools.cpp` | 0.5 day | **Medium** — common agentic task |
| 12 | **Add `run_tests` tool** — Run test suite with structured pass/fail results. | `tools.cpp` | 0.5 day | **Medium** — common agentic task |

### P3 — Polish (Backlog)

| # | Change | File(s) | Effort | Impact |
|---|--------|---------|--------|--------|
| 13 | **Add `web_fetch` tool** — Fetch URL content. | `tools.cpp` | 0.5 day | Low |
| 14 | **Curl timeout matching** — Reduce curl timeout from 3600s to something reasonable (e.g., 120s). | `client.cpp` | 5 min | Low |
| 15 | **SSE parser hardening** — Handle `event:`, `id:`, `retry:` fields explicitly. | `types.cpp` | 0.5 day | Low |
| 16 | **Retry logic cleanup** — Simplify `data_delivered` tracking; add interrupt check during retry sleep. | `client.cpp` | 0.5 day | Low |
| 17 | **Add `memory` / `scratchpad` tool** — Persistent model-accessible storage. | `tools.cpp` | 1 day | Low |

---

## 7. Appendix: Tool Inventory

### Current Tools (7)

| Tool | Parameters | Max Output | Timeout | Plan Mode |
|------|-----------|------------|---------|-----------|
| `list_files` | `path` (string) | unbounded | none | ✓ allowed |
| `read_file` | `path`, `offset` (int), `max_lines` (int) | 400 lines | none | ✓ allowed |
| `grep_files` | `pattern`, `path` (string) | 200 matches | 10s | ✓ allowed |
| `write_file` | `path`, `content` (string) | none | none | ✗ blocked |
| `edit_file` | `path`, `search`, `replace` | brief msg | none | ✗ blocked |
| `run_bash` | `command` (string) | 500 lines / 16k chars | 30s (env) | ✗ blocked |
| `web_search` | `query` (string, max 500 chars) | 10 results | 15s | ✓ allowed |

### Proposed Additions (10)

| Tool | Parameters | Max Output | Timeout | Plan Mode | Priority |
|------|-----------|------------|---------|-----------|----------|
| `apply_patch` | `path`, `patch` (unified diff) | brief msg | 10s | ✗ blocked | P1 |
| `project_tree` | `max_depth` (int, default 5) | 500 lines | 5s | ✓ allowed | P1 |
| `git_status` | none | brief | 10s | ✓ allowed | P1 |
| `git_diff` | `staged` (bool) | 500 lines | 10s | ✓ allowed | P1 |
| `git_log` | `max_count` (int) | 50 commits | 10s | ✓ allowed | P1 |
| `git_commit` | `message` (string), `all` (bool) | brief msg | 10s | ✗ blocked | P1 |
| `delete_file` | `path` (string) | brief msg | none | ✗ blocked | P2 |
| `move_file` | `source`, `destination` (string) | brief msg | none | ✗ blocked | P2 |
| `search_symbols` | `symbol` (string), `path` (string) | 50 matches | 10s | ✓ allowed | P2 |
| `lint` | `path` (string), `tool` (string) | 200 lines | 30s | ✗ blocked | P2 |
| `run_tests` | `path` (string), `filter` (string) | 500 lines | 120s | ✗ blocked | P2 |
| `web_fetch` | `url` (string) | 100k chars | 15s | ✓ allowed | P3 |

---

## Code Quality Observations (Non-Blocking)

1. **Test coverage is excellent** — `test_tools.cpp`, `test_chat.cpp`, `test_client.cpp`, `test_types.cpp`, `test_config.cpp` cover most paths. The mock server (`mock_server.hpp`) is well-designed.

2. **UTF-8 sanitization** — `sanitize_utf8` in `types.cpp` is thorough and correct.

3. **Path sandbox is solid** — `resolve_path` in `tools.cpp` correctly prevents directory traversal, symlink escapes, and absolute-path escapes.

4. **Security** — The `.env` file in the repository contains a live API key. This should be removed from version control and the `.env` entry added to `.gitignore`. **Do not commit API keys.**

5. **Minor:** `gui_chat.cpp` uses `stringstream` throughout where simple string concatenation would suffice. Not a performance issue at UI scale.

6. **Minor:** The `dump()` function in `gui_chat.cpp` (hex dump utility) appears to be unused dead code. Consider removing.

7. **Minor:** `MockServer` uses `std::jthread` (C++20) but the project requires C++23. Consider using `std::thread` for broader C++20 compatibility if needed.

---

*End of Review*
