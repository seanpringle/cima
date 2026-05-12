# Phase 10: Polish

## Goal

Quality-of-life improvements: README, keyboard shortcuts, input auto-focus, error message polish.

## Files

| File | Purpose |
|------|---------|
| `go/README.md` | Project documentation |
| `go/main.go` | Added version print, help flag |
| `go/app/app.go` | Added Ctrl+L, Ctrl+Tab, Ctrl+Shift+Tab, Escape shortcuts |
| `go/app/chat_tab.go` | Added ClearChat(), input auto-focus |
| `go/chat/session.go` | Added friendlyError() for connection/auth/timeout messages |
| `go/tools/worktree.go` | Added CleanupWorktree() exported function |

---

## Step 10.1: README

Created `go/README.md` with:

- Project description
- Requirements (Go 1.24+, Fyne system deps)
- Quick start (`go run .`)
- Configuration table (all env vars)
- Keyboard shortcuts
- Slash commands (/clear, /compact)
- Tool inventory (all 20 tools)
- Architecture diagram
- Development guide (tests, adding tools, code style)
- License (MIT)

---

## Step 10.2: Keyboard Navigation

All shortcuts registered via `fyne.Canvas.AddShortcut()` in `app.setupShortcuts()`:

| Shortcut | Action | Status |
|----------|--------|--------|
| Ctrl+T | New tab | Phase 7 |
| Ctrl+W | Close tab | Phase 7 |
| Ctrl+Q | Quit | Phase 7 |
| **Ctrl+L** | **Clear chat** | **New** |
| **Ctrl+Tab** | **Next tab** | **New** |
| **Ctrl+Shift+Tab** | **Previous tab** | **New** |
| **Escape** | **Cancel running chat** | **New** |
| Ctrl+Enter | Send message | Phase 8 (Fyne default for MultiLine) |

### Focus Management

- Input field auto-focused when chat finishes (`SetRunning(false)` → `canvas.Focus(input)`)
- Input auto-focused after `/clear` and `/compact` (via `ClearChat()`)
- `ClearChat()` method on `ChatWidget` (called by Ctrl+L shortcut)

---

## Step 10.3: Error Messages

`friendlyError()` in `chat/session.go` converts raw errors to user-friendly messages:

| Raw Error | Displayed Message |
|-----------|------------------|
| `connection refused` | `Cannot connect to {API_BASE}. Is the server running?` |
| `no such host` / `lookup` | `Cannot resolve {API_BASE}. Check the LLM_API setting.` |
| `timeout` / `deadline exceeded` | `Request timed out. The model may be overloaded.` |
| `HTTP 401` / `HTTP 403` | `Authentication failed. Check your API key.` |
| `HTTP 429` | `Rate limited. Please wait before sending another request.` |
| `HTTP 5xx` | `Server error at {API_BASE}. The model may be overloaded.` |
| `context canceled` / `interrupted` | `Chat was cancelled.` |
| Other | Raw error message passed through |

---

## Step 10.4: Final Testing

```bash
cd go
go test -count=1 -v ./...
go vet ./...
go test -race -count=1 ./...   # limited to non-network packages
```

### Test Status

All 8 packages pass:
- `cima` (main) — 10 tests
- `cima/app` — 48 tests (Phase 7 + Phase 8 + shortcuts)
- `cima/chat` — 23 tests (including friendlyError coverage)
- `cima/client` — 22 tests
- `cima/config` — 20 tests
- `cima/plan` — 10 tests
- `cima/renderer` — 38 tests
- `cima/tools` — 45+ tests (including worktree lifecycle)

### New Tests Added (Phase 10)

1. **TestSwitchTabNext** — Ctrl+Tab wraps forward
2. **TestSwitchTabPrev** — Ctrl+Shift+Tab wraps backward
3. **TestSwitchTabWrapPrev** — previous from first tab wraps to last
4. **TestClearChatViaWidget** — `ClearChat()` empties entries
5. **TestEscapeShortcutRegistered** — Escape shortcut set up
6. **TestInputAutoFocusAfterChat** — focus restore on chat end
7. **TestHelpFlag** (main) — usage text format
8. **TestVersionSet** (main) — Version is non-empty
9. **TestCleanupWorktreeNoActive** — no panic on inactive
10. **TestWorktreeLifecycle** (main) — git repo fixture

---

## Step 10.5: Code Comments

- All exported types and functions in `app/`, `chat/`, `client/`, `config/`, `plan/`, `renderer/`, `tools/` have doc comments
- `friendlyError` includes inline comments explaining the pattern matching
- Complex logic in `editFile`, `compact`, `RunOnce` has inline comments
- Package-level doc comments added to `config`, `plan`, `renderer`, `main`

---

## Running Phase 10

```bash
cd go
go test -count=1 ./...
go vet ./...
```
