# Phase 8: Chat Tab UI

## Goal

Implement the full chat tab user interface: message display (with markdown rendering), streaming updates, input area, model selector, status bar, and all the interactive controls.

## Files

| File | Purpose |
|------|---------|
| `app/chat_tab.go` | `ChatWidget` — builds the chat tab widget tree |
| `app/chat_tab_test.go` | Tests for chat UI components |

---

## Step 8.1: app/chat_tab.go

### Layout (within each tab's right 60% panel)

```
BorderLayout
├─ Top: Controls bar (toolbar)
│   ├── "Model:" + Select widget (populated async from API)
│   ├── Status label (for model loading errors)
│   ├── Spacer
│   ├── Token count label (right-aligned)
│   ├── " :: "
│   └── Git branch label (right-aligned)
│
├─ Center: Scrollable message list
│   └── VBox of message widgets
│       ├── User message (bold "You: " prefix with icon)
│       ├── Reasoning (gray italic, "Thinking: " prefix)
│       ├── Content (rendered via markdown → RichText)
│       └── Tool call (orange monospace, ellipsis-truncated at 80 chars)
│
└─ Bottom: Input area
    └── widget.Entry (MultiLine, Ctrl+Enter to send, Enter for newline)
```

### Key Functions

```go
package app

// NewChatWidget creates a new chat tab widget with message list, input, and controls.
func NewChatWidget(session *chat.ChatSession, planBoard *plan.PlanBoard) *ChatWidget

// Start begins the background update loop and kicks off async model fetch.
func (cw *ChatWidget) Start()

// Content returns the full chat tab widget tree.
func (cw *ChatWidget) Content() fyne.CanvasObject

// entryToWidget converts a DisplayEntry into a Fyne CanvasObject with proper styling.
func (cw *ChatWidget) entryToWidget(entry chat.DisplayEntry) fyne.CanvasObject
```

### Message Display Area

- A `container.Scroll` containing a `container.VBox`
- Messages are added as they arrive via a buffered channel (capacity 200)
- Each message type has distinct visual styling:
  - **UserText**: bold text with "You: " prefix, left-padded
  - **Reasoning**: gray (`widget.LowImportance`), "Thinking: " prefix, italic
  - **Content**: rendered through `renderer.MarkdownToRichText()` for formatted output
  - **ToolCall**: orange (`widget.WarningImportance`), monospace, truncated with "..." if >80 chars
- Streaming messages: the last Content or Reasoning entry is updated in-place (text merged via `addDisplayEntry` when `IsStreaming` matches same `Seq` and `Type`)
- Auto-scroll: scrolls to bottom when new content arrives, unless the user has scrolled up manually (tracked via `OnScrolled` callback)

### Input Area

- `widget.Entry` with `MultiLine: true` and `Wrapping: fyne.TextWrapWord`
- Placeholder text: "Type a message... (Ctrl+Enter to send, Enter for newline)"
- Fyne's default MultiLineEntry behavior: Enter inserts newline, Ctrl+Enter triggers `OnSubmitted`
- `OnSubmitted` calls `sendInput()`, which sends the current text
- Send button (icon button) also triggers `sendInput()`
- Disabled while a chat is running
- `/clear` command: clears conversation and message display
- `/compact` command: triggers conversation compaction

### Model Selector

- `widget.Select` dropdown in the controls bar
- On `Start()`, launches a goroutine to `session.ClientForModels().FetchModels()`
- While loading: shows "Loading models..." placeholder, selector is disabled
- On error: error text shown in status label
- On success: populates dropdown with model IDs, enables selector
- Selecting a model calls `session.SetModel()` which is propagated to the session
- If the session's current model is in the list, it's pre-selected; otherwise the first model is selected

### Status Bar

Right-aligned in the controls bar:
- **Token count**: `"{total} tokens"`, from `session.LastUsage()`
- **Git branch**: branch name, from `getGitBranch(session.SafeDir())`
- Refreshed on a 100ms timer via `updateLoop()`

### Async Updates

Channel-based pattern:

```go
type DisplayEntry struct {
    Type        EntryType
    Text        string
    IsStreaming bool
    Seq         int
}

// pending is a buffered channel (size 200) from the chat goroutine to the UI.
// A goroutine (updateLoop) reads from it every 100ms and updates the message list.
```

### Failing Tests: `app/chat_tab_test.go`

1. **TestNewChatWidget** — widget created, fields initialised
2. **TestChatWidgetContent** — returns non-nil layout tree
3. **TestAddUserMessage** — user message appears as entry
4. **TestAddAssistantMessage** — assistant content appears
5. **TestAddReasoningMessage** — reasoning entry added
6. **TestAddToolCallMessage** — tool call entry added
7. **TestAddToolCallTruncation** — long tool call text is truncated in rendered widget
8. **TestStreamingContentUpdate** — streaming entry updates in-place (text merged)
9. **TestMultipleMessages** — several messages stored in order
10. **TestInputSendOnEnter** — input accepts text
11. **TestInputClearedAfterSend** — input buffer cleared after sending
12. **TestSlashClearCommand** — `/clear` clears history
13. **TestSlashCompactCommand** — `/compact` triggers compaction
14. **TestInputDisabledWhileRunning** — input disabled during chat
15. **TestDrainPendingUpdates** — pending channel drained correctly with streaming merge
16. **TestDrainPendingMultipleTypes** — multiple entry types drained in order
17. **TestModelSelector** — placeholder shows "Loading models..."
18. **TestModelSelectorInitiallyDisabled** — selector disabled while loading
19. **TestModelSelectorEnabledAfterPopulate** — selector enabled after options set
20. **TestModelSelectorOptionsSet** — options list replaced correctly
21. **TestPlaceholderShowsCtrlEnter** — placeholder mentions Ctrl+Enter
22. **TestEntryToWidgetUserText** — user message widget created
23. **TestEntryToWidgetContent** — content message widget created (via markdown)
24. **TestEntryToWidgetReasoning** — reasoning widget created
25. **TestEntryToWidgetToolCall** — tool call widget created
26. **TestAutoScrollInitialState** — userScrolledUp is false initially
27. **TestRefreshMessagesRebuilds** — messageBox rebuilt after refresh
28. **TestStatusBarLabels** — tokenLabel and branchLabel initialised
29. **TestStatusBarGitBranch** — updateStatusBar does not panic
30. **TestChatWidgetContext** — context returned
31. **TestChatWidgetContextCancel** — context cancelled
32. **TestSetModel** — model changed via selector
33. **TestUpdateTokenDisplay** — token count updated
34. **TestDrainPendingConcurrent** — concurrent drain works correctly

## Running Phase 8 Tests

```bash
cd go
go test ./app/... -v -count=1
```
