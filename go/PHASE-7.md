# Phase 7: GUI Shell

## Goal

Build the Fyne application shell: main window with tab bar, keyboard shortcuts, menu bar, layout scaffolding. This phase focuses on the container structure without wiring the chat logic.

## Files

| File | Purpose |
|------|---------|
| `app/app.go` | `NewApp()` — creates app with tabs, shortcuts, menus |
| `app/app_test.go` | Tests for app setup (testable via `fyne.io/fyne/v2/test`) |

---

## Step 7.1: app/app.go

### Layout

```
Window (size 1280×720, resizable, title "cima")
├─ MainMenu (Model, Debug, Help)
└─ DocTabs
   └─ Each tab → HSplit (40/60)
      ├─ Left: Plan panel (Scroll + monospace Label)
      └─ Right: Chat panel (will be populated in Phase 8)
```

### Types

```go
package app

type ChatTab struct {
    ID         int
    Title      string
    Session    *chat.ChatSession
    PlanBoard  *plan.PlanBoard
    tabItem    *container.TabItem
    planLabel  *widget.Label
    chatWidget *ChatWidget
}

type App struct {
    fyneApp fyne.App
    window  fyne.Window
    tabs    *container.DocTabs
    tabList []*ChatTab
    nextID  int
    cfg     config.Config
}

func NewApp(cfg config.Config, w fyne.Window) *App
func (a *App) AddTab(modelName string)
func (a *App) CloseTab(tab *ChatTab)
func (a *App) ActiveTab() *ChatTab
func (a *App) TabTitles() []string
func (a *App) Content() fyne.CanvasObject
func (a *App) Window() fyne.Window
```

### Initial Tab

When the app starts, create one tab with the default model name (`cfg.Model`).

### Tab Bar

- Use `container.DocTabs` for the tab bar (has built-in close buttons)
- Each tab shows the model name as the title
- Close button (`×`) on each tab — closing the last tab re-adds a default tab
- `OnClosed` callback syncs internal tab list

### Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+T | Add new tab, switch to it |
| Ctrl+W | Close active tab (if >1) |
| Ctrl+Q | Quit app |

Shortcuts are registered via `fyne.Canvas.AddShortcut()` in `setupShortcuts()`.

### Menu Bar

Fyne's `MainMenu` with three menus:

- **Model** menu: stub for model selection (populated in Phase 8)
- **Debug** menu: "Show Raw" toggle stub
- **Help** menu: "About cima" dialog

### Split Layout

Each tab's content is a horizontal 40/60 split:
- Left (40%): scrollable plan document view (monospace `widget.Label`)
- Right (60%): chat widget (populated in Phase 8 via `ChatWidget`)

### Plan Panel

- `widget.Label` with `TextStyle{Monospace: true}` and `Wrapping: fyne.TextWrapWord`
- Content from `PlanBoard.ReadPlan()`
- Initially shows "(empty plan)"

### Failing Tests: `app/app_test.go`

1. **TestNewApp** — app created, tabs initialised
2. **TestInitialTab** — one tab present at startup
3. **TestAddTab** — new tab added, count increases
4. **TestCloseTab** — close tab, count decreases
5. **TestCloseLastTab** — cannot close last tab
6. **TestTabTitles** — tabs show model name
7. **TestWindowTitle** — title is "cima"
8. **TestSplitRatio** — plan panel is 40% width
9. **TestPlanPanelShowsEmpty** — new tab's plan shows "(empty plan)"
10. **TestPlanPanelUsesMonospace** — plan panel has monospace font
11. **TestKeyboardShortcuts** — shortcuts registered
12. **TestDocTabsWithCloseButtons** — DocTabs used (not AppTabs)
13. **TestMenuBarExists** — MainMenu is set with items
14. **TestNewAppWithNilWindow** — nil window handled gracefully

## Running Phase 7 Tests

```bash
cd go
go test ./app/... -v -count=1
```
