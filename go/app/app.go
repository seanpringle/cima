package app

import (
	"cima/chat"
	"cima/config"
	"cima/plan"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/driver/desktop"
	"fyne.io/fyne/v2/widget"
)

// ChatTab represents a single chat tab in the application.
type ChatTab struct {
	ID         int
	Title      string
	Session    *chat.ChatSession
	PlanBoard  *plan.PlanBoard
	tabItem    *container.TabItem
	planLabel  *widget.Label
	chatWidget *ChatWidget
}

// App represents the main application.
type App struct {
	fyneApp fyne.App
	window  fyne.Window
	tabs    *container.DocTabs
	tabList []*ChatTab
	nextID  int
	cfg     config.Config
}

// NewApp creates a new application with the given configuration and window.
// The window is used for keyboard shortcuts and the main menu.
func NewApp(cfg config.Config, w fyne.Window) *App {
	a := &App{
		tabs:    container.NewDocTabs(),
		tabList: make([]*ChatTab, 0),
		nextID:  1,
		cfg:     cfg,
		window:  w,
	}

	// Add the initial tab
	a.AddTab(cfg.Model)

	// Set up UI features
	a.setupShortcuts()
	a.setupMenu()

	// Handle tab close button (DocTabs built-in X button).
	// Remove the tab from our internal list. If it was the last tab,
	// re-add it (the last tab cannot be removed).
	a.tabs.OnClosed = func(ti *container.TabItem) {
		for idx, tab := range a.tabList {
			if tab.tabItem == ti {
				a.tabList = append(a.tabList[:idx], a.tabList[idx+1:]...)
				break
			}
		}
		// If no tabs remain, add one back (last tab cannot be removed)
		if len(a.tabList) == 0 {
			a.AddTab(a.cfg.Model)
		}
	}

	return a
}

// AddTab adds a new tab with the given model name.
func (a *App) AddTab(modelName string) {
	pb := plan.New()
	session := chat.NewSession(a.cfg, pb)
	session.SetModel(modelName)

	tab := &ChatTab{
		ID:    a.nextID,
		Title: modelName,
		Session:   session,
		PlanBoard: pb,
	}
	a.nextID++

	// Create the tab content
	split := a.createTabContent(tab)
	tabItem := container.NewTabItem(modelName, split)
	tab.tabItem = tabItem

	a.tabs.Append(tabItem)
	a.tabs.SelectIndex(len(a.tabList)) // select the new tab (it's appended)
	a.tabList = append(a.tabList, tab)
}

// CloseTab removes a tab. The last tab cannot be removed.
func (a *App) CloseTab(tab *ChatTab) {
	if len(a.tabList) <= 1 {
		return
	}

	for i, t := range a.tabList {
		if t == tab {
			a.tabs.RemoveIndex(i)
			a.tabList = append(a.tabList[:i], a.tabList[i+1:]...)
			break
		}
	}

	// Ensure at least one tab exists
	if len(a.tabList) == 0 {
		a.AddTab(a.cfg.Model)
	}
}

// ActiveTab returns the currently selected tab.
func (a *App) ActiveTab() *ChatTab {
	selectedIdx := a.tabs.SelectedIndex()
	if selectedIdx < 0 || selectedIdx >= len(a.tabList) {
		if len(a.tabList) > 0 {
			return a.tabList[0]
		}
		return nil
	}
	return a.tabList[selectedIdx]
}

// TabTitles returns the titles of all tabs.
func (a *App) TabTitles() []string {
	titles := make([]string, len(a.tabList))
	for i, tab := range a.tabList {
		titles[i] = tab.Title
	}
	return titles
}

// Content returns the main content object (for window setup).
func (a *App) Content() fyne.CanvasObject {
	return a.tabs
}

// Window returns the application window.
func (a *App) Window() fyne.Window {
	return a.window
}

// switchTab moves the selection by delta (+1 = next, -1 = prev), wrapping around.
func (a *App) switchTab(delta int) {
	n := len(a.tabList)
	if n <= 1 {
		return
	}
	cur := a.tabs.SelectedIndex()
	next := (cur + delta) % n
	if next < 0 {
		next += n
	}
	a.tabs.SelectIndex(next)
}

// Shutdown cancels all running chat sessions and cleans up resources.
// Called on window close.
func (a *App) Shutdown() {
	for _, tab := range a.tabList {
		if tab.chatWidget != nil {
			tab.chatWidget.Cancel()
		}
	}
}

// createTabContent builds the 40/60 horizontal split for a tab.
func (a *App) createTabContent(tab *ChatTab) fyne.CanvasObject {
	// Left panel: Plan document viewer (monospace)
	planLabel := widget.NewLabelWithStyle("(empty plan)", fyne.TextAlignLeading, fyne.TextStyle{Monospace: true})
	planLabel.Wrapping = fyne.TextWrapWord
	tab.planLabel = planLabel
	planScroll := container.NewScroll(planLabel)

	// Right panel: Chat tab widget
	chatWidget := NewChatWidget(tab.Session, tab.PlanBoard)
	tab.chatWidget = chatWidget
	chatWidget.Start()

	// Read plan content
	content, err := tab.PlanBoard.ReadPlan()
	if err == nil {
		planLabel.SetText(content)
	}

	// 40/60 horizontal split
	split := container.NewHSplit(
		planScroll,
		chatWidget.Content(),
	)
	split.SetOffset(0.4)

	return split
}

// setupShortcuts registers global keyboard shortcuts.
func (a *App) setupShortcuts() {
	if a.window == nil {
		return
	}
	canvas := a.window.Canvas()

	// Ctrl+T: Add new tab
	ctrlT := &desktop.CustomShortcut{KeyName: fyne.KeyT, Modifier: fyne.KeyModifierControl}
	canvas.AddShortcut(ctrlT, func(shortcut fyne.Shortcut) {
		a.AddTab(a.cfg.Model)
	})

	// Ctrl+W: Close active tab (if >1)
	ctrlW := &desktop.CustomShortcut{KeyName: fyne.KeyW, Modifier: fyne.KeyModifierControl}
	canvas.AddShortcut(ctrlW, func(shortcut fyne.Shortcut) {
		if tab := a.ActiveTab(); tab != nil {
			a.CloseTab(tab)
		}
	})

	// Ctrl+Q: Quit app
	ctrlQ := &desktop.CustomShortcut{KeyName: fyne.KeyQ, Modifier: fyne.KeyModifierControl}
	canvas.AddShortcut(ctrlQ, func(shortcut fyne.Shortcut) {
		if a.window != nil {
			a.window.Close()
		}
	})

	// Ctrl+L: Clear active tab's chat
	ctrlL := &desktop.CustomShortcut{KeyName: fyne.KeyL, Modifier: fyne.KeyModifierControl}
	canvas.AddShortcut(ctrlL, func(shortcut fyne.Shortcut) {
		if tab := a.ActiveTab(); tab != nil && tab.chatWidget != nil {
			tab.chatWidget.ClearChat()
		}
	})

	// Ctrl+Tab: Next tab
	ctrlTab := &desktop.CustomShortcut{KeyName: fyne.KeyTab, Modifier: fyne.KeyModifierControl}
	canvas.AddShortcut(ctrlTab, func(shortcut fyne.Shortcut) {
		a.switchTab(1)
	})

	// Ctrl+Shift+Tab: Previous tab
	ctrlShiftTab := &desktop.CustomShortcut{KeyName: fyne.KeyTab, Modifier: fyne.KeyModifierControl | fyne.KeyModifierShift}
	canvas.AddShortcut(ctrlShiftTab, func(shortcut fyne.Shortcut) {
		a.switchTab(-1)
	})

	// Escape: Cancel running chat
	escape := &desktop.CustomShortcut{KeyName: fyne.KeyEscape}
	canvas.AddShortcut(escape, func(shortcut fyne.Shortcut) {
		if tab := a.ActiveTab(); tab != nil && tab.chatWidget != nil {
			tab.chatWidget.Cancel()
		}
	})
}

// setupMenu creates the main menu bar with stubs.
func (a *App) setupMenu() {
	if a.window == nil {
		return
	}

	modelItem := fyne.NewMenuItem("Model", func() {
		// Placeholder — will be populated in Phase 8 with model selector
		_ = a.ActiveTab()
	})

	debugItem := fyne.NewMenuItem("Show Raw", func() {
		// Placeholder — for debugging raw SSE output
	})

	aboutItem := fyne.NewMenuItem("About cima", func() {
		dialog := widget.NewPopUp(
			widget.NewLabel("cima — AI Coding Assistant\n\nGo + Fyne port"),
			a.window.Canvas(),
		)
		dialog.Show()
	})

	mainMenu := fyne.NewMainMenu(
		fyne.NewMenu("Model", modelItem),
		fyne.NewMenu("Debug", debugItem),
		fyne.NewMenu("Help", aboutItem),
	)

	a.window.SetMainMenu(mainMenu)
}
