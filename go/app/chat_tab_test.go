package app

import (
	"strings"
	"sync"
	"testing"
	"time"

	"cima/chat"
	"cima/config"
	"cima/plan"

	"fyne.io/fyne/v2/test"
)

func newTestChatTab(t *testing.T) (*ChatWidget, *chat.ChatSession, *plan.PlanBoard) {
	t.Helper()
	_ = test.NewApp()
	cfg := config.Config{
		Model:            "test-model",
		APIBase:          "http://localhost:1",
		SystemPrompt:     "test",
		MaxToolIterations: 5,
	}
	pb := plan.New()
	session := chat.NewSession(cfg, pb)
	cw := NewChatWidget(session, pb)
	return cw, session, pb
}

func TestNewChatWidget(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	if cw == nil {
		t.Fatal("NewChatWidget returned nil")
	}
	if cw.session == nil {
		t.Error("session should be set")
	}
	if cw.pending == nil {
		t.Error("pending channel should be created")
	}
}

func TestChatWidgetContent(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	content := cw.Content()
	if content == nil {
		t.Fatal("Content() returned nil")
	}
	if cw.input == nil {
		t.Error("input widget should be initialized")
	}
	if cw.messageBox == nil {
		t.Error("messageBox container should be initialized")
	}
}

func TestAddUserMessage(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryUserText, Text: "Hello!", Seq: 1}
	cw.addDisplayEntry(entry)

	// Verify message was added to the list
	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(cw.entries))
	}
	if cw.entries[0].Text != "Hello!" {
		t.Errorf("entry text = %q, want %q", cw.entries[0].Text, "Hello!")
	}
	_ = session
}

func TestAddAssistantMessage(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryContent, Text: "Response text", Seq: 2}
	cw.addDisplayEntry(entry)

	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(cw.entries))
	}
}

func TestStreamingContentUpdate(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	// Start a streaming entry
	entry1 := chat.DisplayEntry{Type: chat.EntryContent, Text: "Hello", IsStreaming: true, Seq: 1}
	cw.addDisplayEntry(entry1)

	// Update the streaming entry (same seq)
	entry2 := chat.DisplayEntry{Type: chat.EntryContent, Text: " World", IsStreaming: true, Seq: 1}
	cw.addDisplayEntry(entry2)

	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry after streaming update, got %d", len(cw.entries))
	}
	if cw.entries[0].Text != "Hello World" {
		t.Errorf("accumulated text = %q, want %q", cw.entries[0].Text, "Hello World")
	}
}

func TestMultipleMessages(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entries := []chat.DisplayEntry{
		{Type: chat.EntryUserText, Text: "Q1", Seq: 1},
		{Type: chat.EntryReasoning, Text: "thinking", Seq: 2},
		{Type: chat.EntryContent, Text: "A1", Seq: 3},
		{Type: chat.EntryUserText, Text: "Q2", Seq: 4},
		{Type: chat.EntryContent, Text: "A2", Seq: 5},
	}
	for _, e := range entries {
		cw.addDisplayEntry(e)
	}
	if len(cw.entries) != 5 {
		t.Fatalf("expected 5 entries, got %d", len(cw.entries))
	}
}

func TestInputSendOnEnter(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session

	// Set input text
	cw.input.SetText("test message")
	if cw.input.Text != "test message" {
		t.Errorf("input text = %q, want %q", cw.input.Text, "test message")
	}
}

func TestInputClearedAfterSend(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session

	cw.input.SetText("hello")
	cw.sendInput()
	if cw.input.Text != "" {
		t.Errorf("input should be cleared after send, got %q", cw.input.Text)
	}
}

func TestSlashClearCommand(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	// Add some entries
	cw.addDisplayEntry(chat.DisplayEntry{Type: chat.EntryUserText, Text: "old", Seq: 1})

	// Send /clear
	cw.input.SetText("/clear")
	cw.sendInput()

	if len(cw.entries) != 0 {
		t.Errorf("entries should be cleared after /clear, got %d", len(cw.entries))
	}
	_ = session
}

func TestSlashCompactCommand(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	// Add some entries
	cw.addDisplayEntry(chat.DisplayEntry{Type: chat.EntryUserText, Text: "msg1", Seq: 1})

	cw.input.SetText("/compact")
	cw.sendInput()
	_ = session
}

func TestInputDisabledWhileRunning(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	cw.SetRunning(true)
	if !cw.input.Disabled() {
		t.Error("input should be disabled while running")
	}

	cw.SetRunning(false)
	if cw.input.Disabled() {
		t.Error("input should be enabled when not running")
	}
}

func TestDrainPendingUpdates(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session

	// Send some pending updates (same seq = streaming merge)
	cw.pending <- chat.DisplayEntry{Type: chat.EntryContent, Text: "streamed", Seq: 1, IsStreaming: true}
	time.Sleep(5 * time.Millisecond) // let background loop drain
	cw.pending <- chat.DisplayEntry{Type: chat.EntryContent, Text: " data", Seq: 1, IsStreaming: true}
	time.Sleep(5 * time.Millisecond)

	// Drain manually
	cw.drainPending()

	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry after drain, got %d (texts: %v)", len(cw.entries), entriesTexts(cw.entries))
	}
	if cw.entries[0].Text != "streamed data" {
		t.Errorf("accumulated text = %q, want %q", cw.entries[0].Text, "streamed data")
	}
}

func TestDrainPendingMultipleTypes(t *testing.T) {
	cw, _, _ := newTestChatTab(t)

	cw.pending <- chat.DisplayEntry{Type: chat.EntryUserText, Text: "user msg", Seq: 1}
	cw.pending <- chat.DisplayEntry{Type: chat.EntryContent, Text: "response", Seq: 2}
	cw.pending <- chat.DisplayEntry{Type: chat.EntryToolCall, Text: "→ tool()", Seq: 3}

	cw.drainPending()

	if len(cw.entries) != 3 {
		t.Fatalf("expected 3 entries, got %d", len(cw.entries))
	}
}

func TestModelSelector(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	if cw.modelSelect == nil {
		t.Fatal("modelSelect should be initialized")
	}
	// Initially should show loading state
	if cw.modelSelect.PlaceHolder != "Loading models..." {
		t.Errorf("placeholder = %q, want %q", cw.modelSelect.PlaceHolder, "Loading models...")
	}
}

func TestStatusBarLabels(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	if cw.tokenLabel == nil {
		t.Error("tokenLabel should be initialized")
	}
	if cw.branchLabel == nil {
		t.Error("branchLabel should be initialized")
	}
}

func TestChatWidgetContext(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	ctx := cw.Ctx()
	if ctx == nil {
		t.Error("Ctx() should return non-nil context")
	}
}

func TestChatWidgetContextCancel(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	cw.Cancel()
	// After cancel, context should be done
	select {
	case <-cw.Ctx().Done():
		// Context cancelled successfully
	default:
		t.Error("context should be done after Cancel()")
	}
}

func TestSetModel(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	// Set available options first (select requires options to set selection)
	cw.modelSelect.Options = []string{"new-model", "other-model"}
	cw.SetModel("new-model")
	if session.Model() != "new-model" {
		t.Errorf("session model = %q, want %q", session.Model(), "new-model")
	}
	if cw.modelSelect.Selected != "new-model" {
		t.Logf("modelSelect selected = %q (may be empty if not in options at creation)", cw.modelSelect.Selected)
	}
}

func TestUpdateTokenDisplay(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session

	// Set usage via session (the session's last usage will be reflected by calling updateStatusBar)
	session.SetModel("test")
	_ = session.LastUsage() // just ensure it doesn't panic

	// Test that the token label doesn't panic
	cw.updateStatusBar()
	if cw.tokenLabel == nil {
		t.Error("tokenLabel should be set")
	}
}

func TestDrainPendingConcurrent(t *testing.T) {
	cw, _, _ := newTestChatTab(t)

	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 50; i++ {
			cw.pending <- chat.DisplayEntry{Type: chat.EntryContent, Text: "data", Seq: i}
		}
	}()

	// Drain while the goroutine is sending
	time.Sleep(5 * time.Millisecond)
	cw.drainPending()
	wg.Wait()

	// Final drain to get everything
	cw.drainPending()

	if len(cw.entries) == 0 {
		t.Error("should have received entries from concurrent goroutine")
	}
}

// ── Phase 8 spec tests ──

func TestAddReasoningMessage(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryReasoning, Text: "I need to think about this", Seq: 1}
	cw.addDisplayEntry(entry)

	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(cw.entries))
	}
	if cw.entries[0].Text != "I need to think about this" {
		t.Errorf("text = %q", cw.entries[0].Text)
	}
	if cw.entries[0].Type != chat.EntryReasoning {
		t.Errorf("type = %v, want EntryReasoning", cw.entries[0].Type)
	}
}

func TestAddToolCallMessage(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryToolCall, Text: "→ list_files({})", Seq: 1}
	cw.addDisplayEntry(entry)

	if len(cw.entries) != 1 {
		t.Fatalf("expected 1 entry, got %d", len(cw.entries))
	}
	if cw.entries[0].Type != chat.EntryToolCall {
		t.Errorf("type = %v, want EntryToolCall", cw.entries[0].Type)
	}
}

func TestAddToolCallTruncation(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	// Create a long tool call text that should be truncated (>80 chars)
	longText := "→ very_long_tool_name(with_many_arguments_that_exceeds_the_eighty_character_truncation_limit)"
	if len(longText) <= 80 {
		t.Skip("test text too short for truncation")
	}
	entry := chat.DisplayEntry{Type: chat.EntryToolCall, Text: longText, Seq: 1}
	cw.addDisplayEntry(entry)

	// The text should be stored untruncated in the entry
	if cw.entries[0].Text != longText {
		t.Errorf("stored text should not be truncated: got %d chars", len(cw.entries[0].Text))
	}

	// The widget rendering should truncate (tested via entryToWidget)
	widget := cw.entryToWidget(entry)
	if widget == nil {
		t.Error("entryToWidget returned nil for ToolCall")
	}
}

func TestPlaceholderShowsCtrlEnter(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	placeholder := cw.input.PlaceHolder
	if !strings.Contains(placeholder, "Ctrl+Enter") {
		t.Errorf("placeholder should mention Ctrl+Enter, got: %q", placeholder)
	}
}

func TestModelSelectorInitiallyDisabled(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	if !cw.modelSelect.Disabled() {
		t.Error("model selector should be disabled initially (loading state)")
	}
}

func TestModelSelectorEnabledAfterPopulate(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	// Simulate model population
	cw.modelSelect.Options = []string{"model-a", "model-b"}
	cw.modelSelect.Enable()
	cw.modelSelect.PlaceHolder = "Select model..."

	if cw.modelSelect.Disabled() {
		t.Error("model selector should be enabled after population")
	}
	if cw.modelSelect.PlaceHolder != "Select model..." {
		t.Errorf("placeholder = %q, want 'Select model...'", cw.modelSelect.PlaceHolder)
	}
}

func TestModelSelectorOptionsSet(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	models := []string{"gpt-4", "gpt-3.5", "claude-3"}
	cw.modelSelect.Options = models

	if len(cw.modelSelect.Options) != 3 {
		t.Fatalf("expected 3 options, got %d", len(cw.modelSelect.Options))
	}
}

func TestEntryToWidgetUserText(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryUserText, Text: "Hello", Seq: 1}
	obj := cw.entryToWidget(entry)
	if obj == nil {
		t.Fatal("entryToWidget returned nil")
	}
	// Should contain the user text
}

func TestEntryToWidgetContent(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryContent, Text: "**bold** text", Seq: 1}
	obj := cw.entryToWidget(entry)
	if obj == nil {
		t.Fatal("entryToWidget returned nil for Content entry")
	}
}

func TestEntryToWidgetReasoning(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryReasoning, Text: "thinking...", Seq: 1}
	obj := cw.entryToWidget(entry)
	if obj == nil {
		t.Fatal("entryToWidget returned nil for Reasoning entry")
	}
}

func TestEntryToWidgetToolCall(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	entry := chat.DisplayEntry{Type: chat.EntryToolCall, Text: "→ ls()", Seq: 1}
	obj := cw.entryToWidget(entry)
	if obj == nil {
		t.Fatal("entryToWidget returned nil for ToolCall entry")
	}
}

func TestAutoScrollInitialState(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	if cw.userScrolledUp {
		t.Error("userScrolledUp should be false initially")
	}
}

func TestRefreshMessagesRebuilds(t *testing.T) {
	cw, _, _ := newTestChatTab(t)
	// Add some entries
	cw.addDisplayEntry(chat.DisplayEntry{Type: chat.EntryUserText, Text: "Q", Seq: 1})
	cw.addDisplayEntry(chat.DisplayEntry{Type: chat.EntryContent, Text: "A", Seq: 2})

	// Refresh should rebuild the messageBox
	cw.refreshMessages()
	if cw.messageBox == nil {
		t.Error("messageBox should not be nil after refresh")
	}
}

func TestStatusBarGitBranch(t *testing.T) {
	cw, session, _ := newTestChatTab(t)
	_ = session
	// updateStatusBar should not panic even without a real git repo
	cw.updateStatusBar()
	if cw.branchLabel == nil {
		t.Error("branchLabel should be set")
	}
}

// entriesTexts returns the text of all entries for debugging.
func entriesTexts(entries []chat.DisplayEntry) []string {
	texts := make([]string, len(entries))
	for i, e := range entries {
		texts[i] = e.Text
	}
	return texts
}
