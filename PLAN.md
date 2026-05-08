# llm-chat C++23 Rewrite Plan

## Dependencies
- C++23, CMake, Catch2 (system packages)
- nlohmann/json via CMake FetchContent
- libcurl (find_package, REQUIRED)
- GNU readline (find_package, OPTIONAL — fallback to cin/getline)

```bash
sudo apt install libcurl4-openssl-dev libreadline-dev catch2
```

## Project Layout

```
llm-chat/
├── CMakeLists.txt
├── src/
│   ├── main.cpp           # entry point, REPL loop
│   ├── config.h/.cpp      # Config, .env loading, error types
│   ├── client.h/.cpp      # ChatClient, SSEParser — libcurl streaming
│   ├── types.h/.cpp       # Message, ToolCall, ToolAccumulator, Conversation
│   ├── tools.h/.cpp       # Tool, ToolRegistry, sandboxing, tool impls
│   └── chat.h/.cpp        # ChatSession: stream ⇄ tools loop
└── tests/
    ├── mock_server.hpp    # Inline HTTP stub for deterministic tests
    ├── test_config.cpp
    ├── test_types.cpp
    ├── test_tools.cpp
    ├── test_client.cpp    # Uses mock_server, no live endpoint
    └── test_chat.cpp      # Integration: mock + gated live endpoint
```

## Phases

### Phase 1 — Scaffolding + Config + Error types

**Files:** `CMakeLists.txt`, `config.h`, `config.cpp`, `test_config.cpp`

- CMake: C++23, FetchContent(nlohmann), `find_package(CURL REQUIRED)`, `find_package(Readline QUIET)` — optional, compile-time flag `USE_READLINE`
- `Result<T, Error>` type (or `std::expected` equivalent) for recoverable errors; `std::runtime_error` for unrecoverable
- `Config` struct from env vars + `.env` file
- `Config::resolve_safe_dir()` — `std::filesystem::canonical()` on `SAFE_DIR` at load time so sandbox has a stable anchor
- `load_dotenv(path)` — parses `KEY=VALUE`, optional `export` keyword, skips `#` comments and blank lines
- `.env` auto-detect: near `argv[0]` first, then CWD, then `$HOME/.llm-chat.env`

**Tests:**
- Default values with no env
- `.env` parsing (with/without `export`, comments, blanks, quotes)
- Env var override hierarchy
- SAFE_DIR canonicalization

---

### Phase 2 — Types + SSEParser + Conversation

**Files:** `types.h`, `types.cpp`, `test_types.cpp`

- `Message` struct: `role`, `content` (nullable), `reasoning_content`, `optional<ToolCall>`, `tool_call_id`
- `ToolCall` struct: `index`, `id`, `name`, `arguments` (JSON string)
- `ToolAccumulator` — `unordered_map<int, ToolCall>` keyed by `index`. On each SSE delta: `operator[]` creates entry on first sight, then `tool_calls[index].function.arguments += fragment` (string APPEND, not replace). Handles parallel tool calls.
- `SSEParser` — incremental parser that feeds `curl_write_callback(const char*, size_t)`:
  - Buffers partial lines across calls
  - Splits on `\n`, extracts `data: ` prefix
  - Emits callback on complete JSON lines: `on_data(json)`, `on_done()`, `on_error(string)`
- `Conversation` — vector of `Message`, methods: `add_user(text)`, `add_assistant(...)`, `add_tool(...)`, `clear()`, `to_openai_messages()` → JSON array
  - `to_openai_messages()` includes `reasoning_content` when present (custom `nlohmann::json` serialization, not just default)
  - System message always at index 0

**Tests:**
- Message → JSON → Message round-trip (null content, tool_calls, reasoning)
- `ToolAccumulator` with multi-chunk streaming (name in chunk 1, arguments in chunks 2-4, id in chunk 3)
- SSEParser: single-line, multi-line, partial lines across buffers, `[DONE]`
- Conversation JSON array matches expected OpenAI payload shape

---

### Phase 3 — Tool System

**Files:** `tools.h`, `tools.cpp`, `test_tools.cpp`

- `Tool` struct: `name`, `description`, `json parameters_schema`, `function<Result<string>(json args)> execute`
- `ToolRegistry` — vector of `Tool`, `to_openai_tools()`, `execute(name, args_json)` → `Result<string>`
- Path sandbox function (standalone, tested separately):
  - Input: raw path, `safe_dir` (already canonical from Config)
  - `path.lexically_normal()` removes `..` components
  - If path is relative, prepend `safe_dir`
  - `equivalent()` check or `string::starts_with(safe_dir + "/")` with trailing-slash normalization
  - Reject if outside safe_dir
- 5 tools:
  - `list_files(path)` → `ls -la` equivalent via `std::filesystem::directory_iterator`
  - `read_file(path, max_lines=200)` → stream read, line count truncation
  - `grep_files(pattern, path)` → `grep -rn` equivalent via `std::regex` + `std::filesystem::recursive_directory_iterator`, max 50 results
  - `write_file(path, content)` → `create_directories(parent)`, write
  - `run_bash(command)` → `posix_spawn` with new process group (via `setpgid`), capture stdout/stderr via pipes, 30s timeout via `std::jthread` + `wait_for`, `SIGKILL` to process group on timeout, output capped at 100 lines / 4000 chars

**Tests (all in temp directory sandbox):**
- Each tool: valid invocation → correct result
- Path traversal (`../../etc/passwd`) → rejected
- Symlink escape → rejected
- `run_bash` timeout → process killed
- `run_bash` output truncation
- `ToolRegistry::to_openai_tools()` matches expected schema

---

### Phase 4 — HTTP Client

**Files:** `client.h`, `client.cpp`, `mock_server.hpp`, `test_client.cpp`

- `ChatClient` wrapping libcurl:
  - Constructor: `api_base`, `api_key` (optional)
  - `stream_chat(json payload, SSEParser::callbacks)` — `CURLOPT_WRITEFUNCTION` feeds `SSEParser`, `CURLOPT_TIMEOUT` = 120s, `CURLOPT_BUFFERSIZE`, auth header only if key non-empty
  - `chat(json payload)` — non-streaming, returns full JSON response
  - Error: HTTP status check, curl error string, raw response body capture

- `mock_server.hpp`: inline HTTP stub using `socket()` + `bind()` + `listen()` on a random port, returns pre-configured JSON responses (simulates streaming SSE and non-streaming). Runs in a background `std::jthread`.

**Tests (against mock_server):**
- `stream_chat` → callbacks fire with correct deltas
- `chat` non-streaming → correct JSON response
- Auth header present/absent based on key
- Connection error → error returned

**Integration test (gated by `LLM_TEST=1` env var):**
- Hit `127.0.0.1:11000/v1/models` to verify endpoint is alive
- Simple streaming request → non-empty content received

---

### Phase 5 — Conversation Loop

**Files:** `chat.h`, `chat.cpp`, `test_chat.cpp`

- `ChatSession` class:
  - Owns `Conversation`, `ChatClient`, `ToolRegistry`
  - `run_once()`: single user turn
    1. Build payload from conversation + tools
    2. Stream via client, consume SSEParser callbacks:
       - `on_data`: accumulate `content`, `reasoning`, tool call deltas via `ToolAccumulator`
       - On tool_calls present at end: build assistant message with tool_calls, add to conversation, execute each tool, add tool results, loop (max 10 iterations)
       - On content present: flush final content, add assistant message to conversation, return
  - `set_model(string)` — mutable (for `/model` command)
  - `clear()` — reset to just system message

**Tests (against mock_server that returns tool_call response):**
- Simple Q&A → content returned
- Tool call triggered → tool executes → result appended → conversation updated
- 10-iteration guard → terminates
- reasoning_content preserved across turns

---

### Phase 6 — REPL + Main

**Files:** `main.cpp`

- CLI: `/path/to/llm-chat [--model <name>] [--api-base <url>]`
- GNU readline (optional):
  - History: `~/.llm-chat-history`
  - Tab completion for `/exit`, `/clear`, `/model`, `/help`
- Fallback (no readline): `std::cout << "> "`, `std::getline(std::cin, line)`, in-memory `vector<string>` history
- Commands:
  - `/exit` / `/quit` — break
  - `/clear` — `session.clear()`
  - `/model <name>` — `session.set_model(name)`
  - `/help` — command list
- Color: reasoning in `\033[90m` gray, tool invocations stderr `→ tool(args)`, reset on content
- SIGINT: `std::atomic<bool> interrupted` flag, handler calls `_exit(130)` (safe in signal handler)
  - Main loop checks `interrupted` between tool iterations and before streaming
- Welcome banner matching bash version

---

### Phase 7 — Polish

- Curl tuning: `CURLOPT_LOW_SPEED_TIME`, `CURLOPT_TCP_KEEPALIVE`
- Stderr tool logging matching bash format
- Empty response: dump first 500 chars of raw response body for debugging
- `main()`: `try { run(); } catch(const std::exception& e) { cerr << e.what(); return 1; }`
- README with build/test instructions

## Key Design Decisions

| Area | Decision | Rationale |
|---|---|---|
| **SSE** | `SSEParser` class, incremental from curl callback | True streaming, no temp file |
| **Tool call acc.** | `unordered_map<int, ToolCall>` with args string append | Handles parallel calls, multi-chunk args |
| **Path sandbox** | `lexically_normal()` + canonical `safe_dir` prefix check | Correct for nonexistent paths, no throw |
| **run_bash** | `posix_spawn` + process group + `SIGKILL` via jthread | Prevents orphans, clean timeout |
| **Tests** | Mock server for deterministic tests | No live endpoint dependency, safe to run |
| **Readline** | Optional, QUIET find_package, compile-time flag | Portability |
| **Errors** | `Result<T>` for recoverable, exceptions for fatal | Clear ownership, no try/catch noise in hot paths |
| **reasoning_content** | Custom JSON serialization in Conversation | Non-standard field required by DeepSeek API |

## Integration Test Gate

Tests tagged `[.integration]` in Catch2, guarded by:
```c++
if (!std::getenv("LLM_TEST")) SKIP("set LLM_TEST=1 to run integration tests");
```

## Build Commands

```bash
cd llm-chat
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -j$(nproc)
LLM_TEST=1 ctest --test-dir build -j$(nproc) -E 'unit'  # integration only
```
