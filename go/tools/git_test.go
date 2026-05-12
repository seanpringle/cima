package tools

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	gogit "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/plumbing/object"
)

// createTestRepo creates a temporary git repository with an initial commit.
func createTestRepo(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()

	repo, err := gogit.PlainInit(dir, false)
	if err != nil {
		t.Fatalf("PlainInit: %v", err)
	}

	// Create an initial file and commit
	err = os.WriteFile(filepath.Join(dir, "README.md"), []byte("# Test\n"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	wt, err := repo.Worktree()
	if err != nil {
		t.Fatalf("Worktree: %v", err)
	}

	_, err = wt.Add("README.md")
	if err != nil {
		t.Fatalf("Add: %v", err)
	}

	_, err = wt.Commit("Initial commit", &gogit.CommitOptions{
		Author: &object.Signature{Name: "Test", Email: "test@test.com"},
	})
	if err != nil {
		t.Fatalf("Commit: %v", err)
	}

	return dir
}

func TestGitStatusClean(t *testing.T) {
	safeDir := createTestRepo(t)
	tool := makeGitStatusTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "no changes") {
		t.Errorf("clean repo should show no changes: %q", result)
	}
}

func TestGitStatusModified(t *testing.T) {
	safeDir := createTestRepo(t)

	// Modify the file
	err := os.WriteFile(filepath.Join(safeDir, "README.md"), []byte("# Modified\n"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	tool := makeGitStatusTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "M") {
		t.Errorf("result should show modified status: %q", result)
	}
}

func TestGitStatusUntracked(t *testing.T) {
	safeDir := createTestRepo(t)

	// Create an untracked file
	err := os.WriteFile(filepath.Join(safeDir, "new.txt"), []byte("new"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	tool := makeGitStatusTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "?") {
		t.Errorf("result should show untracked status: %q", result)
	}
}

func TestGitDiffUnstaged(t *testing.T) {
	safeDir := createTestRepo(t)

	// Modify file
	err := os.WriteFile(filepath.Join(safeDir, "README.md"), []byte("# Modified content\n"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	tool := makeGitDiffTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Modified") {
		t.Errorf("diff should contain modified content: %q", result)
	}
}

func TestGitDiffStaged(t *testing.T) {
	safeDir := createTestRepo(t)

	// Modify and stage
	err := os.WriteFile(filepath.Join(safeDir, "README.md"), []byte("# Staged change\n"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	repo, err := gogit.PlainOpen(safeDir)
	if err != nil {
		t.Fatalf("PlainOpen: %v", err)
	}
	wt, err := repo.Worktree()
	if err != nil {
		t.Fatalf("Worktree: %v", err)
	}
	_, err = wt.Add("README.md")
	if err != nil {
		t.Fatalf("Add: %v", err)
	}

	tool := makeGitDiffTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"staged": true})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Staged") {
		t.Errorf("diff should contain staged content: %q", result)
	}
}

func TestGitDiffNoUnstaged(t *testing.T) {
	safeDir := createTestRepo(t)

	tool := makeGitDiffTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "no unstaged changes") {
		t.Errorf("clean repo should show no unstaged changes: %q", result)
	}
}

func TestGitLogBasic(t *testing.T) {
	safeDir := createTestRepo(t)

	tool := makeGitLogTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Initial commit") {
		t.Errorf("log should contain 'Initial commit': %q", result)
	}
}

func TestGitLogOneline(t *testing.T) {
	safeDir := createTestRepo(t)

	tool := makeGitLogTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"format": "oneline"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Initial commit") {
		t.Errorf("log should contain 'Initial commit': %q", result)
	}
	// Oneline format should be short: hash + subject
	lines := strings.Split(strings.TrimSpace(result), "\n")
	if len(lines) < 1 {
		t.Error("expected at least 1 line of output")
	}
}

func TestGitLogMaxCount(t *testing.T) {
	safeDir := createTestRepo(t)

	tool := makeGitLogTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"max_count": float64(1)})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Initial commit") {
		t.Errorf("log should contain 'Initial commit': %q", result)
	}
}

func TestGitAddAndCommit(t *testing.T) {
	safeDir := createTestRepo(t)

	// Create a new file
	err := os.WriteFile(filepath.Join(safeDir, "newfile.txt"), []byte("content"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	// Add it
	addTool := makeGitAddTool(&safeDir)
	result, err := addTool.Execute(context.Background(), map[string]any{"path": "newfile.txt"})
	if err != nil {
		t.Fatalf("Add Execute: %v", err)
	}
	if !strings.Contains(result, "staged") {
		t.Errorf("add result should mention 'staged': %q", result)
	}

	// Commit it
	commitTool := makeGitCommitTool(&safeDir)
	result, err = commitTool.Execute(context.Background(), map[string]any{"message": "Add newfile"})
	if err != nil {
		t.Fatalf("Commit Execute: %v", err)
	}
	if !strings.Contains(result, "committed") {
		t.Errorf("commit result should mention 'committed': %q", result)
	}
}

func TestGitCommitAll(t *testing.T) {
	safeDir := createTestRepo(t)

	// Create a new file (no explicit add)
	err := os.WriteFile(filepath.Join(safeDir, "auto.txt"), []byte("auto-added"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	// Commit with all:true
	commitTool := makeGitCommitTool(&safeDir)
	result, err := commitTool.Execute(context.Background(), map[string]any{
		"message": "Auto commit",
		"all":     true,
	})
	if err != nil {
		t.Fatalf("Commit Execute: %v", err)
	}
	if !strings.Contains(result, "committed") {
		t.Errorf("commit result should mention 'committed': %q", result)
	}
}

func TestGitCommitEmptyMessage(t *testing.T) {
	safeDir := createTestRepo(t)

	commitTool := makeGitCommitTool(&safeDir)
	_, err := commitTool.Execute(context.Background(), map[string]any{"message": ""})
	if err == nil {
		t.Fatal("expected error for empty commit message")
	}
}

func TestGitAddAll(t *testing.T) {
	safeDir := createTestRepo(t)

	err := os.WriteFile(filepath.Join(safeDir, "added.txt"), []byte("content"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	err = os.WriteFile(filepath.Join(safeDir, "added2.txt"), []byte("content2"), 0644)
	if err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	addTool := makeGitAddTool(&safeDir)
	result, err := addTool.Execute(context.Background(), map[string]any{"all": true})
	if err != nil {
		t.Fatalf("Add Execute: %v", err)
	}
	if !strings.Contains(result, "staged all") {
		t.Errorf("add result should mention 'staged all': %q", result)
	}
}

func TestGetCurrentGitBranch(t *testing.T) {
	safeDir := createTestRepo(t)

	branch, err := GetCurrentGitBranch(safeDir)
	if err != nil {
		t.Fatalf("GetCurrentGitBranch: %v", err)
	}
	if branch == "" {
		t.Error("branch should not be empty")
	}
}

func TestOpenGitRepoNotARepo(t *testing.T) {
	dir := t.TempDir()
	_, _, err := OpenGitRepo(dir)
	if err == nil {
		t.Fatal("expected error for non-repo directory")
	}
}
