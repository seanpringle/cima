# cima — AI Coding Assistant (Go/Fyne port)

A desktop LLM agent application with tool-calling loop, git integration, and a
Plan document workflow. Clean-room reimplementation of the original C++/ImGui
version in Go with the Fyne GUI toolkit.

## Requirements

- **Go 1.24+** — installed from Debian/apt (package `golang-1.24` or from [go.dev](https://go.dev))
- **Fyne system dependencies** — `libgl-dev`, `libegl-dev`, `libxcursor-dev`,
  `libxi-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxxf86vm-dev`
  ```bash
  sudo apt install libgl-dev libegl-dev libxcursor-dev libxi-dev libxrandr-dev \
      libxinerama-dev libxxf86vm-dev
  ```
- **git** — for worktree operations, repository browsing

## Quick Start

```bash
cd go

# Set your API endpoint
export LLM_API=https://api.openai.com/v1
export LLM_KEY=sk-your-key-here

# Run
go run .
```

The application starts with one chat tab. Type a message and press **Ctrl+Enter** to send. The assistant can use tools (read/write files, run bash, search the web, git operations, manage a Plan document).

## Configuration

All configuration is via environment variables:

| Variable | Default | Description |
|----------|---------|-------------|
| `LLM_API` | `http://127.0.0.1:11000/v1` | API endpoint (fallback: `API_BASE`) |
| `LLM_KEY` | — | API key (fallback: `API_KEY`) |
| `MODEL` | `deepseek-v4-flash` | Model name |
| `LLM_REASONING_EFFORT` | `high` | Reasoning depth |
| `SAFE_DIR` | current directory | Sandbox for tool operations |
| `LLM_MAX_TOOL_ITERATIONS` | `100` | Max tool-call loop iterations |
| `LLM_CONTEXT_LIMIT` | `300000` | Context window in tokens |
| `LLM_COMPACT_THRESHOLD` | `90` | Compaction trigger (%) |
| `LLM_SYSTEM_PROMPT` | built-in | Override system prompt |
| `WORKTREE_BASE` | `/tmp/cima` | Parent dir for git worktrees |
| `READ_ONLY_PATHS` | `/usr/include:/usr/share/doc` | Extra read-accessible paths |
| `SEARCH_API_KEY` | — | Google Custom Search API key |
| `SEARCH_ENGINE_ID` | — | Google Custom Search engine ID |
| `SEARCH_ENDPOINT` | — | Custom search endpoint URL |

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| **Ctrl+Enter** | Send message |
| **Enter** | Newline (in input area) |
| **Ctrl+T** | New tab |
| **Ctrl+W** | Close tab |
| **Ctrl+Q** | Quit |
| **Ctrl+L** | Clear chat |
| **Ctrl+Tab** | Next tab |
| **Ctrl+Shift+Tab** | Previous tab |
| **Escape** | Cancel running chat |

## Slash Commands

Type these in the chat input:

| Command | Action |
|---------|--------|
| `/clear` | Clear conversation history |
| `/compact` | Compact conversation (removes old tool rounds) |

## Tools

The assistant has access to ~20 tools:

| Category | Tools |
|----------|-------|
| **Filesystem** | `list_files`, `project_tree` |
| **File I/O** | `read_file`, `read_file_lines`, `write_file`, `edit_file`, `delete_file`, `move_file`, `rename_file` |
| **Bash** | `run_bash` (30s timeout, output capped) |
| **Search** | `grep_files` (regex, max 200 results) |
| **Web** | `web_search` (DDG / Google CSE), `web_fetch` (cached) |
| **Git** | `git_status`, `git_diff`, `git_log`, `git_add`, `git_commit` |
| **Worktree** | `start_worktree`, `stop_worktree` |
| **Plan** | `write_plan`, `read_plan`, `comment_plan` |

## Architecture

```
go/
├── main.go               Entry point, flags, validation, cleanup
├── app/                  Fyne GUI (App, tabs, ChatWidget)
├── chat/                 Conversation, Session (tool-loop orchestrator)
├── client/               OpenAI-compatible HTTP client + SSE parser
├── config/               Environment-variable config loading
├── plan/                 PlanBoard document store
├── renderer/             Markdown → Fyne RichText (goldmark)
└── tools/                Agent tools (filesystem, bash, git, web, etc.)
```

## Development

### Run tests

```bash
cd go
go test ./... -v -count=1
```

### Run with race detector

```bash
cd go
go test -race -count=1 ./...
```

### Add a new tool

1. Create the tool function in `go/tools/` (e.g. `my_tool.go`)
2. Wrap it with a `makeMyTool()` function returning a `tools.Tool` struct
3. Register it in `AddDefaults()` or call `registry.Add()` in session setup
4. Add tests in the same package

### Code style

- Doc comments on all exported types and functions
- Error handling via `*tools.ToolError` for user-facing tool errors
- Tests use `httptest` for HTTP mocking, apply table-driven patterns
- Use `filepath` for all path operations (cross-platform)

## License

MIT
