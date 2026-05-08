# llm-chat

Interactive LLM chat client with tool calling, streaming, and reasoning display.
## Dependencies

```bash
sudo apt install build-essential cmake libcurl4-openssl-dev libreadline-dev catch2
```

- C++23 compiler (g++ 14+)
- CMake 3.25+
- libcurl (any SSL backend)
- GNU readline _(optional — fallback to plain `cin`)_
- nlohmann/json _(fetched via CMake FetchContent)_
- Catch2 _(for tests)_

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary is `build/llm-chat`.

## Usage

```bash
./build/llm-chat [--model <name>] [--api-base <url>]
```

### Interactive commands

| Command | Description |
|---|---|
| `/exit` / `/quit` | Exit |
| `/clear` | Reset conversation (keeps system prompt) |
| `/model <name>` | Switch model at runtime |
| `/help` | Show commands |

### Configuration

Environment variables (highest priority), `.env` file, then defaults:

| Variable | Default | Description |
|---|---|---|
| `LLM_API` / `API_BASE` | `http://127.0.0.1:11000/v1` | API endpoint |
| `LLM_KEY` / `API_KEY` | _(none)_ | Bearer token |
| `MODEL` | `deepseek-v4-flash` | Model name |
| `SYSTEM_PROMPT` | `You are a helpful assistant.` | System prompt |
| `SAFE_DIR` | `$(pwd)` | Tool sandbox directory |

`.env` auto-detection order: exe directory → CWD → `$HOME/.llm-chat.env`.

### Tools

Five tools are available to the model inside `SAFE_DIR`:
- `list_files` — list directory contents
- `read_file` — read file (max 200 lines)
- `grep_files` — search file contents (regex, max 50 results)
- `write_file` — write file (creates parent dirs)
- `run_bash` — execute shell command (capped at 100 lines / 4000 chars, 30s timeout)

Path traversal (`../../etc/passwd`) and symlink escapes are rejected.

## Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build
```

66 unit tests covering config, types, SSE parsing, conversation, tool sandbox,
HTTP client, and full conversation loop (with inline mock server).

Integration tests against a live endpoint:

```bash
LLM_TEST=1 ctest --test-dir build
```

## Architecture

```
src/
  main.cpp     — REPL with optional readline
  config.h/.cpp    — Env/.env loading, Config struct
  types.h/.cpp     — Message, ToolCall, SSEParser, Conversation
  tools.h/.cpp     — Tool/ToolRegistry, 5 tools, path sandbox
  client.h/.cpp    — ChatClient wrapping libcurl (streaming + non-streaming)
  chat.h/.cpp      — ChatSession: tool call loop (max 10 iterations)

tests/
  mock_server.hpp  — Inline HTTP stub for deterministic tests
  test_*.cpp       — One per module
```

## Project status

| Phase | What | Status |
|---|---|---|
| 1 | CMake scaffold + Config + .env | ✅ |
| 2 | Types, SSEParser, Conversation | ✅ |
| 3 | Tool system, path sandbox, run_bash | ✅ |
| 4 | HTTP client + mock server | ✅ |
| 5 | Conversation loop (stream→tool→stream) | ✅ |
| 6 | REPL + readline + CLI args | ✅ |
| 7 | Polish + README | ✅ |
