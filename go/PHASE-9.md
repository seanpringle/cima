# Phase 9: Integration

## Goal

Wire everything together into a working application. `main.go` is the entry point that loads config, handles flags, performs start-up validation, creates the Fyne app, initialises all subsystems, and launches the GUI. Cleanup on shutdown cancels active chats and removes worktrees.

## Files

| File | Purpose |
|------|---------|
| `main.go` | Entry point — flags, config, validation, GUI launch, cleanup |
| `main_test.go` | Integration tests (httptest mock API + git CLI fixtures) |

---

## Step 9.1: main.go

### Structure

```go
package main

import (
    "flag"
    "fmt"
    "os"

    "cima/app"
    "cima/config"
    "cima/tools"

    "fyne.io/fyne/v2"
    fyneApp "fyne.io/fyne/v2/app"
)

var Version = "dev"

func main() {
    flag.Parse()
    // --help flag → print usage, exit 0

    cfg := config.FromEnv()

    // Validate APIBase, check SAFE_DIR, create worktree base

    // Fyne app + dark theme
    fa := fyneApp.NewWithID("cima")
    fa.Settings().SetTheme(theme.DarkTheme())

    // Window + App
    window := fa.NewWindow("cima")
    window.Resize(fyne.NewSize(1280, 720))
    appInstance := app.NewApp(cfg, window)
    window.SetContent(appInstance.Content())

    // Cleanup on close
    window.SetCloseIntercept(func() {
        appInstance.Shutdown()
        tools.CleanupWorktree()
        window.Close()
    })

    window.ShowAndRun()
}
```

### Behaviour

1. **Flag parsing**: `--help` / `-h` prints usage text (lists env vars) and exits 0
2. **Config loading**: `config.FromEnv()` loads all settings from environment variables
3. **Start-up validation**:
   - Prints version string to stderr on start
   - Validates `APIBase` is set (fatal if missing)
   - Checks `SAFE_DIR` exists (warning if not, only when explicitly set)
   - Creates worktree base directory if missing
4. **Fyne app creation**: Uses `fyne.CurrentApp()` with fallback to `fyneApp.NewWithID("cima")`
5. **Window setup**: Title "cima", default size 1280×720, dark theme
6. **App creation**: `app.NewApp(cfg, window)` creates tabs, shortcuts, menus internally
7. **Cleanup on quit** (`SetCloseIntercept`):
   - `appInstance.Shutdown()` — calls `Cancel()` on all active `ChatWidget` contexts
   - `tools.CleanupWorktree()` — removes any active worktree directory
   - Then closes the window
8. **Event loop**: `window.ShowAndRun()`

### Subsystem Wiring

Tabs, shortcuts, and menus are handled internally by `app.App`:

- `app.NewApp(cfg, window)` creates the initial tab with a `ChatSession` + `PlanBoard`
- `app.AddTab(modelName)` adds additional tabs
- Shortcuts (Ctrl+T, Ctrl+W, Ctrl+Q) are set up by `app.setupShortcuts()`
- Menu bar (Model, Debug, Help) is set up by `app.setupMenu()`
- `app.Shutdown()` iterates all tabs and cancels their chat widget contexts

### Cleanup Functions

```go
package app

// Shutdown cancels all running chat sessions in all tabs.
func (a *App) Shutdown()
```

```go
package tools

// CleanupWorktree removes any active worktree directory.
// Called on application shutdown. Best-effort, errors are silently ignored.
func CleanupWorktree()
```

---

## Step 9.2: End-to-End Tests

### Tests: `main_test.go`

1. **TestConfigLoad** — `config.FromEnv()` completes without panic, APIBase is non-empty
2. **TestFullRoundTrip** — mock API server, create session, `RunOnce("hello")`, verify response content
3. **TestFullRoundTripWithToolCall** — mock API returns tool call → tool executes → final answer returned
4. **TestModelChangeAndSession** — change model mid-session, verify next request uses new model
5. **TestClientStreaming** — direct client streaming, verifies content is assembled across chunks
6. **TestGracefulShutdown** — cancel context mid-stream, verify RunOnce returns error
7. **TestHelpFlag** — verify usage text contains expected sections
8. **TestVersionSet** — verify `Version` variable is non-empty
9. **TestCleanupWorktreeNoActive** — `CleanupWorktree()` does not panic when no worktree active
10. **TestWorktreeLifecycle** — basic git repo fixture creation (expand as needed)

### Test Strategy

- All HTTP tests use `net/http/httptest` to mock the OpenAI-compatible API
- Fyne is set up via `test.NewApp()` from `fyne.io/fyne/v2/test` (no display needed)
- Git operations use the system `git` CLI on temporary repos
- No real API keys or network calls are made

---

## Running Phase 9 Tests

```bash
cd go
go test ./... -v -count=1
```
