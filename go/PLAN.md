# cima — Port from C++/ImGui to Go/Fyne

## Goal

Clean-room reimplementation of the **cima** desktop LLM agent application from C++ (Dear ImGui + SDL3 + libcurl + libgit2 + md4c) to **Go** with the **Fyne** GUI toolkit, using only the Go standard library plus a small set of well-established Go packages.

## Architecture

```
go/
├── main.go                 # Entry point, config loading
├── app/
│   ├── app.go              # Fyne application setup, window, tab bar
│   └── chat_tab.go         # Per-tab chat widget (messages + input + plan)
├── client/
│   ├── client.go           # OpenAI-compatible HTTP client (streaming + non-streaming)
│   └── sse.go              # SSE streaming parser
├── chat/
│   ├── session.go          # ChatSession — orchestrates LLM→tools→LLM loop
│   ├── conversation.go     # Conversation history with compaction
│   └── types.go            # Message, ToolCall, Usage, DisplayEntry, ToolAccumulator
├── plan/
│   └── planboard.go        # Per-session Plan document (write/read/comment)
├── tools/
│   ├── registry.go         # ToolRegistry — register, discover, execute tools
│   ├── tool.go             # Tool type + ToolPermission
│   ├── path.go             # Path sandboxing (resolve against safe_dir)
│   ├── git_helpers.go      # Git helper functions (open repo, branch name, gitignore)
│   ├── web_helpers.go      # HTTP GET helper + cache for web tools
│   ├── worktree.go         # start_worktree / stop_worktree tools
│   ├── filesystem.go       # list_files, project_tree
│   ├── file_io.go          # read_file, read_file_lines, write_file, edit_file,
│   │                       # delete_file, move_file, rename_file
│   ├── bash.go             # run_bash
│   ├── grep.go             # grep_files
│   ├── web.go              # web_search, web_fetch
│   ├── git.go              # git_status, git_diff, git_log, git_add, git_commit
│   └── plan_tools.go       # write_plan, read_plan, comment_plan
├── renderer/
│   └── markdown.go         # Markdown → Fyne rich-text rendering
├── config/
│   └── config.go           # Config struct + env-var loading
└── go.mod
```

## Dependencies

| Package | Purpose | Go get |
|---------|---------|--------|
| `fyne.io/fyne/v2` | Desktop GUI toolkit | `go get fyne.io/fyne/v2` |
| `github.com/go-git/go-git/v5` | Pure-Go git operations | `go get github.com/go-git/go-git/v5` |
| `github.com/yuin/goldmark` | Markdown → AST parsing | `go get github.com/yuin/goldmark` |
| stdlib `net/http` | HTTP client | Built-in |
| stdlib `encoding/json` | JSON encoding | Built-in |
| stdlib `os/exec` | Running bash | Built-in |
| stdlib `context` | Cancellation | Built-in |

All dependencies are fetched by `go get` and stored in `$GOPATH/pkg/mod`. No system-wide package installation is needed beyond what is already present on this Debian system (Go 1.24.4, libgl-dev, libegl-dev, libxcursor-dev, libxi-dev, libxrandr-dev, libxinerama-dev, libxxf86vm-dev).

## Key Design Differences from C++ Version

| C++ | Go |
|-----|-----|
| Dear ImGui (immediate mode) | Fyne (retained widget tree) |
| SDL3 window/renderer | Fyne internal OpenGL window |
| libcurl HTTP client | `net/http` with `io.Copy` |
| nlohmann/json | `encoding/json` |
| libgit2 (C library) | `go-git/go-git` (pure Go) |
| md4c Markdown parser | goldmark AST + Fyne RichText |
| `std::shared_ptr<std::atomic<bool>>` cancellation | `context.Context` + `context.WithCancel` |
| `std::expected<T, string>` | `(T, error)` idiom |
| `std::thread` / `std::async` | goroutines + channels |
| `std::mutex` | `sync.Mutex` |
| Catch2 test framework | `testing` stdlib package |

## Development Approach

Test-driven development throughout:

1. Write a **failing test** that describes the desired behaviour
2. Implement the minimal code to make it pass
3. Refactor, add more tests

Each phase builds on the previous one and includes both unit tests (for the package) and integration tests (for cross-package behaviour). The final phase wires everything together into a working GUI.

## Phases

| Phase | Files | Description |
|-------|-------|-------------|
| 1 | `config/config.go`, `chat/types.go`, `plan/planboard.go` | Core types, Config, PlanBoard |
| 2 | `client/sse.go`, `client/client.go` | HTTP client + SSE parser |
| 3 | `chat/conversation.go` | Conversation history + compaction |
| 4 | All `tools/*.go` | All ~20 agent tools |
| 5 | `chat/session.go` | Chat session orchestrator |
| 6 | `renderer/markdown.go` | Markdown → Fyne rendering |
| 7 | `app/app.go` | Window shell, tabs, menu |
| 8 | `app/chat_tab.go` | Chat tab UI, message display, input |
| 9 | Integration | End-to-end wiring, `main.go` |
| 10 | Polish | Fonts, error messages, final testing |

## Go Version

Go 1.24.4 from Debian is installed and sufficient. Go's module system (`go mod init`, `go mod tidy`, `go build`) keeps all dependencies self-contained in `$GOPATH/pkg/mod`. No root or system-wide setup is required beyond the already-installed graphics development packages.
