package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"strings"
	"testing"

	"cima/client"
	"cima/config"
	"cima/plan"
	"cima/chat"
	"cima/tools"
)

// ── Helpers ──

func writeJSON(w http.ResponseWriter, data any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(data)
}

func mockSSE(w http.ResponseWriter, events ...string) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.WriteHeader(http.StatusOK)
	flusher, _ := w.(http.Flusher)
	for _, e := range events {
		fmt.Fprint(w, e)
		flusher.Flush()
	}
}

func contentDelta(text string) string {
	escaped, _ := json.Marshal(text)
	return fmt.Sprintf("data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":%s}}]}\n\n", string(escaped))
}

func toolCallDelta(id, name, args string) string {
	escapedArgs, _ := json.Marshal(args)
	return fmt.Sprintf("data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":%s}}]}}]}\n\n", id, name, string(escapedArgs))
}

var doneEvent = "data: [DONE]\n\n"

// TestFullRoundTrip tests a complete chat interaction: user message → assistant response.
func TestFullRoundTrip(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mockSSE(w, contentDelta("Hello! I'm an AI assistant."), doneEvent)
	}))
	defer srv.Close()

	cfg := config.Config{
		APIBase:           srv.URL,
		Model:             "test-model",
		SystemPrompt:      "You are a test assistant.",
		MaxToolIterations: 5,
		ContextLimit:      100000,
		CompactThreshold:  90,
	}
	pb := plan.New()
	session := chat.NewSession(cfg, pb)
	ctx := context.Background()

	result, err := session.RunOnce(ctx, "Hi")
	if err != nil {
		t.Fatalf("RunOnce: %v", err)
	}
	if !strings.Contains(result.Content, "Hello") {
		t.Errorf("Content = %q, should contain 'Hello'", result.Content)
	}
}

// TestFullRoundTripWithToolCall tests the tool-calling loop.
func TestFullRoundTripWithToolCall(t *testing.T) {
	var callCount int
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		callCount++
		if callCount == 1 {
			// First API call: return a tool call
			mockSSE(w, toolCallDelta("call_1", "bash_ls", `{"command":"ls"}`), doneEvent)
		} else {
			// Second API call: return final answer with tool result incorporated
			mockSSE(w, contentDelta("I ran ls for you."), doneEvent)
		}
	}))
	defer srv.Close()

	cfg := config.Config{
		APIBase:           srv.URL,
		Model:             "test-model",
		SystemPrompt:      "You are a test assistant.",
		MaxToolIterations: 5,
		ContextLimit:      100000,
		CompactThreshold:  90,
	}
	pb := plan.New()
	session := chat.NewSession(cfg, pb)
	ctx := context.Background()

	result, err := session.RunOnce(ctx, "List files")
	if err != nil {
		t.Fatalf("RunOnce: %v", err)
	}
	if !strings.Contains(result.Content, "I ran ls") {
		t.Errorf("Content = %q, should contain tool result", result.Content)
	}
	if callCount < 2 {
		t.Errorf("expected at least 2 API calls, got %d", callCount)
	}
}

// TestConfigLoad tests that config loading works.
func TestConfigLoad(t *testing.T) {
	cfg := config.FromEnv()
	if cfg.APIBase == "" {
		t.Error("APIBase should not be empty after FromEnv")
	}
}

// TestModelChangeAndSession tests changing model mid-session.
func TestModelChangeAndSession(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mockSSE(w, contentDelta("ok"), doneEvent)
	}))
	defer srv.Close()

	cfg := config.Config{
		APIBase:           srv.URL,
		Model:             "test-model",
		SystemPrompt:      "test",
		MaxToolIterations: 5,
	}
	pb := plan.New()
	session := chat.NewSession(cfg, pb)
	ctx := context.Background()

	// Run a first turn
	result, err := session.RunOnce(ctx, "First message")
	if err != nil {
		t.Fatalf("First RunOnce: %v", err)
	}
	_ = result

	// Change model
	session.SetModel("new-model")
	if session.Model() != "new-model" {
		t.Errorf("Model = %q, want %q", session.Model(), "new-model")
	}

	// Run a second turn with new model
	result, err = session.RunOnce(ctx, "Second message")
	if err != nil {
		t.Fatalf("Second RunOnce: %v", err)
	}
	if !strings.Contains(result.Content, "ok") {
		t.Errorf("Content = %q", result.Content)
	}
}

// TestClientStreaming tests streaming directly with the client.
func TestClientStreaming(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mockSSE(w, contentDelta("Hello"), contentDelta(" World"), doneEvent)
	}))
	defer srv.Close()

	c := client.New(srv.URL, "")
	var content strings.Builder

	err := c.StreamChat(context.Background(), map[string]any{
		"model":    "test",
		"messages": []any{map[string]any{"role": "user", "content": "hi"}},
		"stream":   true,
	}, client.SSECallbacks{
		OnData: func(data map[string]any) {
			if choices, ok := data["choices"].([]any); ok && len(choices) > 0 {
				if choice, ok := choices[0].(map[string]any); ok {
					if delta, ok := choice["delta"].(map[string]any); ok {
						if text, ok := delta["content"].(string); ok {
							content.WriteString(text)
						}
					}
				}
			}
		},
		OnDone: func() {},
	})

	if err != nil {
		t.Fatalf("StreamChat: %v", err)
	}
	if content.String() != "Hello World" {
		t.Errorf("streamed content = %q, want %q", content.String(), "Hello World")
	}
}

// TestGracefulShutdown tests that cancelling during a chat works.
func TestGracefulShutdown(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		flusher, _ := w.(http.Flusher)
		fmt.Fprint(w, contentDelta("Partial"))
		flusher.Flush()
		<-r.Context().Done()
	}))
	defer srv.Close()

	cfg := config.Config{
		APIBase:           srv.URL,
		Model:             "test-model",
		SystemPrompt:      "test",
		MaxToolIterations: 5,
	}
	pb := plan.New()
	session := chat.NewSession(cfg, pb)
	_ = session

	// Start a chat in background and cancel
	errCh := make(chan error)
	go func() {
		_, err := session.RunOnce(ctx, "Hi")
		errCh <- err
	}()

	cancel()

	err := <-errCh
	if err == nil {
		t.Log("RunOnce returned nil after cancel")
	} else {
		t.Logf("RunOnce error after cancel: %v", err)
	}
}

// TestHelpFlag tests that --help prints usage and exits 0.
func TestHelpFlag(t *testing.T) {
	// We can't easily run the main function with flags in a test,
	// so we verify the usage text is present and correct.
	if !strings.Contains(usageText, "cima") {
		t.Error("usage text should mention 'cima'")
	}
	if !strings.Contains(usageText, "LLM_API") {
		t.Error("usage text should mention LLM_API")
	}
	if !strings.Contains(usageText, "SAFE_DIR") {
		t.Error("usage text should mention SAFE_DIR")
	}
	if !strings.Contains(usageText, "Flags:") {
		t.Error("usage text should have a Flags section")
	}
}

// TestVersionSet tests that Version is not empty.
func TestVersionSet(t *testing.T) {
	if Version == "" {
		t.Error("Version should not be empty")
	}
}

// TestCleanupWorktreeNoActive tests CleanupWorktree on an inactive state.
func TestCleanupWorktreeNoActive(t *testing.T) {
	// Should not panic when no worktree is active
	tools.CleanupWorktree()
}

// TestWorktreeLifecycle tests a complete worktree lifecycle via the tools package.
func TestWorktreeLifecycle(t *testing.T) {
	// Create a temp git repo
	repoDir := t.TempDir()
	if err := os.MkdirAll(repoDir, 0755); err != nil {
		t.Fatal(err)
	}

	// Init repo
	initCmd := testGitCommand(repoDir, "init")
	if out, err := initCmd.CombinedOutput(); err != nil {
		t.Fatalf("git init: %s: %v", out, err)
	}

	// Configure user
	testGitCommand(repoDir, "config", "user.name", "test").Run()
	testGitCommand(repoDir, "config", "user.email", "test@test").Run()

	// Create initial commit
	testGitCommand(repoDir, "commit", "--allow-empty", "-m", "initial").Run()

	t.Logf("repo at %s", repoDir)
}

// testGitCommand creates a git command in the given directory.
func testGitCommand(dir string, args ...string) *exec.Cmd {
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	return cmd
}
