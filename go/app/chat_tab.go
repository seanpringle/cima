package app

import (
	"bytes"
	"context"
	"fmt"
	"os/exec"
	"strings"
	"sync"
	"time"

	"cima/chat"
	"cima/plan"
	"cima/renderer"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/layout"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"
)

// ChatWidget is the chat tab's right-panel UI (message list + input + controls).
type ChatWidget struct {
	session   *chat.ChatSession
	planBoard *plan.PlanBoard

	// UI widgets
	messageBox    *fyne.Container
	messageScroll *container.Scroll
	input         *widget.Entry
	modelSelect   *widget.Select
	tokenLabel    *widget.Label
	branchLabel   *widget.Label
	sendBtn       *widget.Button
	statusLabel   *widget.Label // for model loading status

	// State
	entries     []chat.DisplayEntry
	pending     chan chat.DisplayEntry
	running     bool
	seqCounter  int
	mu          sync.Mutex
	ctx         context.Context
	cancel      context.CancelFunc

	// Auto-scroll tracking
	userScrolledUp bool
}

// NewChatWidget creates a new chat tab widget.
func NewChatWidget(session *chat.ChatSession, pb *plan.PlanBoard) *ChatWidget {
	ctx, cancel := context.WithCancel(context.Background())

	cw := &ChatWidget{
		session:   session,
		planBoard: pb,
		entries:   make([]chat.DisplayEntry, 0),
		pending:   make(chan chat.DisplayEntry, 200),
		ctx:       ctx,
		cancel:    cancel,
	}

	// Model selector (initially empty, populated asynchronously)
	cw.modelSelect = widget.NewSelect([]string{}, func(selected string) {
		if selected != "" && selected != session.Model() {
			cw.SetModel(selected)
		}
	})
	cw.modelSelect.PlaceHolder = "Loading models..."
	cw.modelSelect.Disable()

	// Status label for model loading errors
	cw.statusLabel = widget.NewLabel("")
	cw.statusLabel.Hide()

	// Token count and branch labels
	cw.tokenLabel = widget.NewLabel("0 tokens")
	cw.tokenLabel.Alignment = fyne.TextAlignTrailing

	cw.branchLabel = widget.NewLabel("")
	cw.branchLabel.Alignment = fyne.TextAlignTrailing

	// Message list (empty container, filled dynamically)
	cw.messageBox = container.NewVBox()
	cw.messageScroll = container.NewScroll(cw.messageBox)

	// Track scroll position for auto-scroll behavior
	cw.messageScroll.OnScrolled = func(pos fyne.Position) {
		// Check if we're at the bottom (within a small epsilon)
		contentHeight := cw.messageBox.MinSize().Height
		scrollHeight := cw.messageScroll.Size().Height
		maxScroll := contentHeight - scrollHeight
		if maxScroll < 0 {
			maxScroll = 0
		}
		cw.userScrolledUp = pos.Y < maxScroll-5
	}

	// Input area
	cw.input = widget.NewMultiLineEntry()
	cw.input.SetPlaceHolder("Type a message... (Ctrl+Enter to send, Enter for newline)")
	cw.input.OnSubmitted = func(text string) {
		// OnSubmitted fires on Ctrl+Enter for MultiLineEntry (Fyne default)
		cw.sendInput()
	}

	// Send button
	cw.sendBtn = widget.NewButtonWithIcon("", theme.MailSendIcon(), func() {
		cw.sendInput()
	})

	return cw
}

// Start begins the background update loop for UI refresh and kicks off
// the async model list fetch. Must be called after the widget is part of
// the window tree.
func (cw *ChatWidget) Start() {
	go cw.updateLoop()
	go cw.fetchModels()
}

// Content returns the full chat tab widget tree.
func (cw *ChatWidget) Content() fyne.CanvasObject {
	// Controls bar: model selector + debug + status
	controls := cw.buildControls()

	// Input area: entry + send button
	inputBox := container.NewBorder(
		nil, nil, nil, cw.sendBtn,
		cw.input,
	)

	// Full layout: controls on top, messages in center, input at bottom
	return container.NewBorder(
		controls,   // top
		inputBox,   // bottom
		nil, nil,   // left, right
		cw.messageScroll, // center
	)
}

// Ctx returns the context for this widget (used for cancellation).
func (cw *ChatWidget) Ctx() context.Context {
	return cw.ctx
}

// Cancel cancels the current operation.
func (cw *ChatWidget) Cancel() {
	cw.cancel()
}

// SetModel changes the model and updates the UI.
func (cw *ChatWidget) SetModel(model string) {
	cw.session.SetModel(model)
	if cw.modelSelect != nil {
		cw.modelSelect.SetSelected(model)
	}
}

// ClearChat clears the conversation and message display (Ctrl+L).
func (cw *ChatWidget) ClearChat() {
	cw.session.Clear()
	cw.mu.Lock()
	cw.entries = nil
	cw.mu.Unlock()
	cw.refreshMessages()
	cw.input.SetText("")
	// Focus via driver canvas
	if canvas := fyne.CurrentApp().Driver().CanvasForObject(cw.input); canvas != nil {
		canvas.Focus(cw.input)
	}
}

// SetRunning enables/disables the input area based on chat running state.
func (cw *ChatWidget) SetRunning(running bool) {
	cw.mu.Lock()
	cw.running = running
	cw.mu.Unlock()

	if running {
		cw.input.Disable()
		cw.sendBtn.Disable()
	} else {
		cw.input.Enable()
		cw.sendBtn.Enable()
		// Auto-focus input after chat finishes
		if canvas := fyne.CurrentApp().Driver().CanvasForObject(cw.input); canvas != nil {
			canvas.Focus(cw.input)
		}
	}
}

// addDisplayEntry adds a display entry to the message list, merging with the
// last entry if it's a streaming continuation (same seq and type).
func (cw *ChatWidget) addDisplayEntry(entry chat.DisplayEntry) {
	cw.mu.Lock()
	defer cw.mu.Unlock()

	if len(cw.entries) > 0 {
		last := &cw.entries[len(cw.entries)-1]
		if last.Seq == entry.Seq && last.Type == entry.Type && last.IsStreaming {
			last.Text += entry.Text
			return
		}
		// If the last entry was streaming but this is a different type/seq,
		// mark it as not streaming
		if last.IsStreaming {
			last.IsStreaming = false
		}
	}

	cw.entries = append(cw.entries, entry)
}

// drainPending processes all pending entries from the channel.
func (cw *ChatWidget) drainPending() {
	for {
		select {
		case entry := <-cw.pending:
			cw.addDisplayEntry(entry)
		default:
			return
		}
	}
}

// fetchModels asynchronously loads the model list from the API.
func (cw *ChatWidget) fetchModels() {
	client := cw.session.ClientForModels()
	if client == nil {
		return
	}

	models, err := client.FetchModels(cw.ctx)
	if err != nil {
		fyne.Do(func() {
			cw.mu.Lock()
			if cw.statusLabel != nil {
				cw.statusLabel.SetText("Model fetch failed")
				cw.statusLabel.Show()
			}
			cw.mu.Unlock()
		})
		return
	}

	if len(models) == 0 {
		return
	}

	fyne.Do(func() {
		cw.modelSelect.Options = models
		cw.modelSelect.Enable()
		cw.modelSelect.PlaceHolder = "Select model..."

		currentModel := cw.session.Model()
		for _, m := range models {
			if m == currentModel {
				cw.modelSelect.SetSelected(currentModel)
				return
			}
		}
		if len(models) > 0 {
			cw.modelSelect.SetSelected(models[0])
			cw.session.SetModel(models[0])
		}
	})
}

// sendInput sends the current input text to the chat session.
func (cw *ChatWidget) sendInput() {
	text := strings.TrimSpace(cw.input.Text)
	if text == "" {
		return
	}
	cw.input.SetText("")

	// Handle commands
	if text == "/clear" {
		cw.session.Clear()
		cw.mu.Lock()
		cw.entries = nil
		cw.mu.Unlock()
		cw.refreshMessages()
		return
	}
	if text == "/compact" {
		cw.session.Compact()
		cw.addDisplayEntry(chat.DisplayEntry{
			Type: chat.EntryContent,
			Text: "[⌂ compaction]",
		})
		cw.refreshMessages()
		return
	}

	// Add user message to display
	cw.mu.Lock()
	cw.seqCounter++
	seq := cw.seqCounter
	cw.mu.Unlock()
	cw.addDisplayEntry(chat.DisplayEntry{
		Type: chat.EntryUserText,
		Text: text,
		Seq:  seq,
	})
	cw.refreshMessages()

	// Start the async chat via the session
	cw.SetRunning(true)
	go cw.runChat(text)
}

// runChat runs the chat in a goroutine, sending updates to the pending channel.
func (cw *ChatWidget) runChat(userInput string) {
	defer func() {
		fyne.Do(func() { cw.SetRunning(false) })
	}()

	cw.mu.Lock()
	cw.seqCounter++
	seq := cw.seqCounter
	cw.mu.Unlock()

	// Set up output callback to send entries to the pending channel
	cw.session.SetOutputCallback(func(text string, entryType chat.EntryType) {
		cw.pending <- chat.DisplayEntry{
			Type:        entryType,
			Text:        text,
			IsStreaming: true,
			Seq:         seq,
		}
	})

	// Run the chat session
	_, err := cw.session.RunOnce(cw.ctx, userInput)
	if err != nil {
		cw.pending <- chat.DisplayEntry{
			Type: chat.EntryContent,
			Text: "Error: " + err.Error(),
			Seq:  seq,
		}
	}
}

// updateLoop periodically drains pending entries and refreshes the UI.
func (cw *ChatWidget) updateLoop() {
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			cw.drainPending()
			fyne.Do(func() {
				cw.refreshMessages()
				cw.updateStatusBar()
			})
		case <-cw.ctx.Done():
			return
		}
	}
}

// refreshMessages rebuilds the message container from entries.
func (cw *ChatWidget) refreshMessages() {
	cw.mu.Lock()
	entries := make([]chat.DisplayEntry, len(cw.entries))
	copy(entries, cw.entries)
	cw.mu.Unlock()

	cw.messageBox.Objects = nil

	for _, entry := range entries {
		obj := cw.entryToWidget(entry)
		cw.messageBox.Add(obj)

		// Add spacing between messages
		if false { // skip for the last item, but VBox handles this
			cw.messageBox.Add(widget.NewSeparator())
		}
	}

	// Auto-scroll: only if user hasn't scrolled up
	if !cw.userScrolledUp && len(entries) > 0 {
		cw.messageScroll.ScrollToBottom()
	}
	cw.messageBox.Refresh()
}

// entryToWidget converts a DisplayEntry to a Fyne CanvasObject with proper styling.
func (cw *ChatWidget) entryToWidget(entry chat.DisplayEntry) fyne.CanvasObject {
	switch entry.Type {
	case chat.EntryUserText:
		rt := renderer.MarkdownToRichText("**You:** " + entry.Text)
		rt.Wrapping = fyne.TextWrapWord
		return rt

	case chat.EntryReasoning:
		rt := renderer.MarkdownToRichText("*Thinking:* " + entry.Text)
		rt.Wrapping = fyne.TextWrapWord
		return rt

	case chat.EntryContent:
		// Render via markdown for formatted output
		rt := renderer.MarkdownToRichText(entry.Text)
		rt.Wrapping = fyne.TextWrapWord
		return rt

	case chat.EntryToolCall:
		text := entry.Text
		if len(text) > 80 {
			text = text[:80] + "..."
		}
		rt := renderer.MarkdownToRichText("`" + text + "`")
		rt.Wrapping = fyne.TextWrapWord
		return rt

	default:
		label := widget.NewLabel(entry.Text)
		label.Wrapping = fyne.TextWrapWord
		return label
	}
}

// buildControls creates the top control bar for the chat tab.
func (cw *ChatWidget) buildControls() fyne.CanvasObject {
	// Model selector with label
	modelLabel := widget.NewLabel("Model:")
	modelBox := container.NewHBox(modelLabel, cw.modelSelect, cw.statusLabel)

	// Status bar (right-aligned)
	statusBox := container.NewHBox(
		layout.NewSpacer(),
		cw.tokenLabel,
		widget.NewLabelWithStyle(" :: ", fyne.TextAlignCenter, fyne.TextStyle{}),
		cw.branchLabel,
	)

	return container.NewBorder(
		nil, nil,
		modelBox,     // left
		statusBox,    // right
		layout.NewSpacer(), // center
	)
}

// getGitBranch returns the current git branch name for the given directory.
func getGitBranch(dir string) (string, error) {
	var stdout, stderr bytes.Buffer
	cmd := exec.Command("git", "branch", "--show-current")
	cmd.Dir = dir
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return "", err
	}
	return strings.TrimSpace(stdout.String()), nil
}

// updateStatusBar refreshes token count and git branch.
func (cw *ChatWidget) updateStatusBar() {
	usage := cw.session.LastUsage()
	cw.tokenLabel.SetText(fmt.Sprintf("%d tokens", usage.TotalTokens))

	// Try to get current git branch
	safeDir := cw.session.SafeDir()
	if safeDir != "" {
		branch, err := getGitBranch(safeDir)
		if err == nil && branch != "" {
			cw.branchLabel.SetText(branch)
		} else {
			cw.branchLabel.SetText("")
		}
	}
}
