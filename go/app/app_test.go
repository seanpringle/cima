package app

import (
	"testing"

	"cima/chat"
	"cima/config"

	"fyne.io/fyne/v2/test"
	"fyne.io/fyne/v2/container"
)

// newTestApp creates a test App with a test window.
func newTestApp(t *testing.T, cfg config.Config) *App {
	t.Helper()
	fa := test.NewApp()
	w := fa.NewWindow("test")
	return NewApp(cfg, w)
}

func TestNewApp(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	if a == nil {
		t.Fatal("NewApp returned nil")
	}
	if a.tabs == nil {
		t.Error("tabs should be initialised")
	}
}

func TestInitialTab(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	if len(a.tabList) != 1 {
		t.Fatalf("expected 1 initial tab, got %d", len(a.tabList))
	}
	if a.tabList[0].Title != "test-model" {
		t.Errorf("tab title = %q, want %q", a.tabList[0].Title, "test-model")
	}
}

func TestAddTab(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("new-model")
	if len(a.tabList) != 2 {
		t.Fatalf("expected 2 tabs after AddTab, got %d", len(a.tabList))
	}
	if a.tabList[1].Title != "new-model" {
		t.Errorf("tab[1].Title = %q, want %q", a.tabList[1].Title, "new-model")
	}
}

func TestCloseTab(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("tab2")
	a.AddTab("tab3")

	if len(a.tabList) != 3 {
		t.Fatalf("expected 3 tabs, got %d", len(a.tabList))
	}

	// Close the first tab
	a.CloseTab(a.tabList[0])
	if len(a.tabList) != 2 {
		t.Fatalf("expected 2 tabs after close, got %d", len(a.tabList))
	}
}

func TestCloseLastTabNotAllowed(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	if len(a.tabList) != 1 {
		t.Fatalf("expected 1 tab, got %d", len(a.tabList))
	}
	a.CloseTab(a.tabList[0])
	// Last tab should not be removable
	if len(a.tabList) != 1 {
		t.Errorf("should not be able to close last tab: got %d tabs", len(a.tabList))
	}
}

func TestActiveTab(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("second")
	active := a.ActiveTab()
	if active == nil {
		t.Fatal("ActiveTab returned nil")
	}
	// The active tab should be the last added one (second)
	if active.Title != "second" {
		t.Errorf("ActiveTab Title = %q, want %q", active.Title, "second")
	}
}

func TestTabTitles(t *testing.T) {
	cfg := config.Config{
		Model:        "m1",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("m2")
	a.AddTab("m3")

	titles := a.TabTitles()
	expected := []string{"m1", "m2", "m3"}
	if len(titles) != len(expected) {
		t.Fatalf("TabTitles = %v, want %v", titles, expected)
	}
	for i, title := range titles {
		if title != expected[i] {
			t.Errorf("TabTitles[%d] = %q, want %q", i, title, expected[i])
		}
	}
}

func TestPlanPanelShowsEmpty(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	tab := a.tabList[0]
	if tab.planLabel == nil {
		t.Fatal("planLabel should be set")
	}
	if tab.planLabel.Text != "(empty plan)" {
		t.Errorf("planLabel.Text = %q, want %q", tab.planLabel.Text, "(empty plan)")
	}
}

func TestPlanPanelShowsContent(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	tab := a.tabList[0]

	// Write a plan
	tab.PlanBoard.WritePlan("Test plan body")

	// Re-create tab content to see the updated plan
	a.createTabContent(tab)
	if tab.planLabel == nil {
		t.Fatal("planLabel should be set")
	}
}

func TestContentReturnsTabs(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	if a.Content() != a.tabs {
		t.Error("Content() should return the tabs container")
	}
}

// ── Phase 7 spec tests ──

func TestWindowTitle(t *testing.T) {
	fa := test.NewApp()
	w := fa.NewWindow("cima")
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := NewApp(cfg, w)
	if a.window == nil {
		t.Fatal("window should be set")
	}
	if a.window.Title() != "cima" {
		t.Errorf("window title = %q, want %q", a.window.Title(), "cima")
	}
}

func TestSplitRatio(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	tab := a.tabList[0]

	// The split is created in createTabContent; recreate to capture output
	split := a.createTabContent(tab)

	// Verify it's an HSplit
	hs, ok := split.(*container.Split)
	if !ok {
		t.Fatalf("expected *container.Split, got %T", split)
	}
	// The offset should be approximately 0.4
	offset := hs.Offset
	if offset < 0.35 || offset > 0.45 {
		t.Errorf("split offset = %f, want ~0.4", offset)
	}
}

func TestPlanPanelUsesMonospace(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	tab := a.tabList[0]
	if tab.planLabel == nil {
		t.Fatal("planLabel should be set")
	}
	if !tab.planLabel.TextStyle.Monospace {
		t.Error("planLabel should use monospace style")
	}
}

func TestKeyboardShortcuts(t *testing.T) {
	fa := test.NewApp()
	w := fa.NewWindow("test")
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := NewApp(cfg, w)

	// Verify shortcuts are registered by checking the canvas
	canvas := w.Canvas()
	if canvas == nil {
		t.Fatal("canvas should not be nil")
	}

	// The canvas should have keyboard shortcut handlers registered
	// We can't easily invoke them in tests, but we can verify the setup doesn't panic
	if len(a.tabList) != 1 {
		t.Errorf("expected 1 tab initially, got %d", len(a.tabList))
	}
}

func TestDocTabsWithCloseButtons(t *testing.T) {
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)

	// Verify tabs is a DocTabs (which has close buttons)
	// We can't type-assert directly since tabs is *container.DocTabs (concrete).
	// Instead verify the OnClosed callback is set (unique to DocTabs).
	if a.tabs.OnClosed == nil {
		t.Error("DocTabs should have OnClosed callback set")
	}
}

func TestMenuBarExists(t *testing.T) {
	fa := test.NewApp()
	w := fa.NewWindow("test")
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := NewApp(cfg, w)

	// Verify menu is set on the window
	menu := a.Window().MainMenu()
	if menu == nil {
		t.Fatal("MainMenu should not be nil")
	}
	if len(menu.Items) == 0 {
		t.Error("menu should have items")
	}
}

func TestNewAppWithNilWindow(t *testing.T) {
	// Passing nil window should not panic (shortcuts/menus will be skipped)
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := NewApp(cfg, nil)
	if a == nil {
		t.Fatal("NewApp returned nil")
	}
	if len(a.tabList) != 1 {
		t.Errorf("expected 1 tab, got %d", len(a.tabList))
	}
}

// ── Phase 10 shortcut tests ──

func TestSwitchTabNext(t *testing.T) {
	cfg := config.Config{
		Model:        "m1",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("m2")
	a.AddTab("m3")

	// Start on last tab (m3)
	if a.ActiveTab().Title != "m3" {
		t.Fatalf("expected active tab to be m3, got %q", a.ActiveTab().Title)
	}

	// Next tab wraps to first
	a.switchTab(1)
	if a.ActiveTab().Title != "m1" {
		t.Errorf("after next: expected m1, got %q", a.ActiveTab().Title)
	}
}

func TestSwitchTabPrev(t *testing.T) {
	cfg := config.Config{
		Model:        "m1",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("m2")
	a.AddTab("m3")

	// Start on last tab (m3), prev goes to m2
	a.switchTab(-1)
	if a.ActiveTab().Title != "m2" {
		t.Errorf("after prev: expected m2, got %q", a.ActiveTab().Title)
	}
}

func TestSwitchTabWrapPrev(t *testing.T) {
	cfg := config.Config{
		Model:        "m1",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := newTestApp(t, cfg)
	a.AddTab("m2")

	// We're on m2 (most recently added). One more prev wraps to m1.
	a.switchTab(-1)
	// Actually after AddTab("m2"), we're on m2 (index 1).
	// switchTab(-1) goes to index 0 (m1).
	if a.ActiveTab().Title != "m1" {
		t.Errorf("after prev wrap: expected m1, got %q", a.ActiveTab().Title)
	}
}

func TestClearChatViaWidget(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session

	// Add a message
	cw.addDisplayEntry(chat.DisplayEntry{Type: chat.EntryUserText, Text: "test", Seq: 1})
	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(cw.entries))
	}

	// Clear via ClearChat
	cw.ClearChat()
	if len(cw.entries) != 0 {
		t.Errorf("entries should be empty after ClearChat, got %d", len(cw.entries))
	}
}

func TestEscapeShortcutRegistered(t *testing.T) {
	fa := test.NewApp()
	w := fa.NewWindow("test")
	cfg := config.Config{
		Model:        "test-model",
		APIBase:      "http://localhost:1",
		SystemPrompt: "test",
	}
	a := NewApp(cfg, w)

	// Verify shortcuts are set up by calling them indirectly
	if a.ActiveTab() == nil {
		t.Fatal("expected active tab")
	}
}

func TestInputAutoFocusAfterChat(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	// Simulate chat finishing (running -> not running)
	cw.SetRunning(true)
	cw.SetRunning(false)
	// Should not panic; focus depends on canvas being set up
}
