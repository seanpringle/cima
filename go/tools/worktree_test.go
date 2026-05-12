package tools

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// setupTestRepo creates a temporary git repository with an initial commit.
// Returns the repo directory path and a cleanup function.
func setupTestRepo(t *testing.T) (string, func()) {
	t.Helper()
	dir := t.TempDir()

	// Initialize git repo
	cmd := exec.Command("git", "init")
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git init failed: %s: %v", out, err)
	}

	// Configure user for commits
	for _, kv := range []string{"user.name test", "user.email test@test"} {
		parts := strings.SplitN(kv, " ", 2)
		cmd = exec.Command("git", "config", parts[0], parts[1])
		cmd.Dir = dir
		_ = cmd.Run()
	}

	// Create an initial commit
	readme := filepath.Join(dir, "README.md")
	if err := os.WriteFile(readme, []byte("# Test\n"), 0644); err != nil {
		t.Fatalf("write README: %v", err)
	}
	cmd = exec.Command("git", "add", "README.md")
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git add: %s: %v", out, err)
	}
	cmd = exec.Command("git", "commit", "-m", "initial")
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git commit: %s: %v", out, err)
	}

	return dir, func() {
		os.RemoveAll(dir)
	}
}

// TestStartWorktree creates a worktree and verifies the directory exists.
func TestStartWorktree(t *testing.T) {
	repoDir, cleanup := setupTestRepo(t)
	defer cleanup()

	worktreeBase := t.TempDir()
	safeDir := repoDir
	tool := makeStartWorktreeTool(&safeDir, worktreeBase)

	result, err := tool.Execute(context.Background(), map[string]any{
		"branch": "test-worktree",
	})
	if err != nil {
		t.Fatalf("start_worktree: %v", err)
	}
	if !strings.Contains(result, "test-worktree") {
		t.Errorf("result should mention branch name: %q", result)
	}
	if !strings.Contains(result, "ok") {
		t.Errorf("result should start with 'ok': %q", result)
	}
	// Verify worktree directory was created
	if _, err := os.Stat(safeDir); os.IsNotExist(err) {
		t.Error("worktree directory should exist")
	}
	// Verify safeDir points to worktree now
	if !strings.Contains(safeDir, "test-worktree") {
		t.Errorf("safeDir should point to worktree path, got %q", safeDir)
	}

	// Clean up: stop worktree
	stopTool := makeStopWorktreeTool(&safeDir, &repoDir)
	_, err = stopTool.Execute(context.Background(), map[string]any{"force": true})
	if err != nil {
		t.Fatalf("stop_worktree cleanup: %v", err)
	}
}

func TestStartWorktreeAlreadyActive(t *testing.T) {
	repoDir, cleanup := setupTestRepo(t)
	defer cleanup()

	worktreeBase := t.TempDir()
	safeDir := repoDir
	tool := makeStartWorktreeTool(&safeDir, worktreeBase)

	// Start first worktree
	_, err := tool.Execute(context.Background(), map[string]any{
		"branch": "worktree-1",
	})
	if err != nil {
		t.Fatalf("first start_worktree: %v", err)
	}

	// Try starting another worktree while one is active
	_, err = tool.Execute(context.Background(), map[string]any{
		"branch": "worktree-2",
	})
	if err == nil {
		t.Error("expected error when starting worktree while one is active")
	}
	if !strings.Contains(err.Error(), "already active") {
		t.Errorf("error should mention 'already active', got: %v", err)
	}

	// Clean up
	stopTool := makeStopWorktreeTool(&safeDir, &repoDir)
	_, _ = stopTool.Execute(context.Background(), map[string]any{"force": true})
}

func TestStopWorktreeNoActive(t *testing.T) {
	repoDir := t.TempDir()
	safeDir := repoDir
	tool := makeStopWorktreeTool(&safeDir, &repoDir)

	_, err := tool.Execute(context.Background(), map[string]any{})
	if err == nil {
		t.Error("expected error when stopping without active worktree")
	}
	if !strings.Contains(err.Error(), "no active worktree") {
		t.Errorf("error should mention 'no active worktree', got: %v", err)
	}
}

func TestWorktreeSafeDirSwitching(t *testing.T) {
	repoDir, cleanup := setupTestRepo(t)
	defer cleanup()

	originalSafeDir := repoDir
	worktreeBase := t.TempDir()
	safeDir := originalSafeDir

	startTool := makeStartWorktreeTool(&safeDir, worktreeBase)

	_, err := startTool.Execute(context.Background(), map[string]any{
		"branch": "feature-branch",
	})
	if err != nil {
		t.Fatalf("start_worktree: %v", err)
	}

	// After start, safeDir should point to worktree (not original repo)
	if safeDir == originalSafeDir {
		t.Error("safeDir should have changed after start_worktree")
	}
	if !strings.Contains(safeDir, "feature-branch") {
		t.Errorf("safeDir should contain branch name, got %q", safeDir)
	}

	worktreePath := safeDir
	// Restore
	stopTool := makeStopWorktreeTool(&safeDir, &originalSafeDir)
	_, err = stopTool.Execute(context.Background(), map[string]any{"force": true})
	if err != nil {
		t.Fatalf("stop_worktree: %v", err)
	}

	// After stop, safeDir should be restored to original
	if safeDir != originalSafeDir {
		t.Errorf("safeDir should be restored to %q, got %q", originalSafeDir, safeDir)
	}
	// Worktree directory should be gone
	if _, err := os.Stat(worktreePath); !os.IsNotExist(err) {
		t.Error("worktree directory should have been removed")
	}
}

func TestWorktreeBranchNameRequired(t *testing.T) {
	repoDir, cleanup := setupTestRepo(t)
	defer cleanup()

	worktreeBase := t.TempDir()
	safeDir := repoDir
	tool := makeStartWorktreeTool(&safeDir, worktreeBase)

	_, err := tool.Execute(context.Background(), map[string]any{})
	if err == nil {
		t.Error("expected error when branch name is empty")
	}
}

func TestWorktreeStartInNonRepo(t *testing.T) {
	worktreeBase := t.TempDir()
	safeDir := t.TempDir() // Not a git repo
	tool := makeStartWorktreeTool(&safeDir, worktreeBase)

	_, err := tool.Execute(context.Background(), map[string]any{
		"branch": "test-branch",
	})
	if err == nil {
		t.Error("expected error when not in a git repository")
	}
	if !strings.Contains(err.Error(), "not inside a git repository") {
		t.Errorf("error should mention not a git repo, got: %v", err)
	}
}

func TestSanitizeBranchName(t *testing.T) {
	tests := []struct {
		input    string
		expected string
	}{
		{"simple", "simple"},
		{"feature/my-thing", "feature-my-thing"},
		{"branch with spaces", "branch-with-spaces"},
		{"dots.and.slashes", "dots-and-slashes"},
		{"weird\\path", "weird-path"},
	}
	for _, tt := range tests {
		result := sanitizeBranchName(tt.input)
		if result != tt.expected {
			t.Errorf("sanitizeBranchName(%q) = %q, want %q", tt.input, result, tt.expected)
		}
	}
}
