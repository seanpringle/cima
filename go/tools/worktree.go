package tools

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
)

// WorktreeState tracks the current state of a git worktree session.
type WorktreeState struct {
	mu              sync.Mutex
	Active          bool
	OriginalSafeDir string
	WorktreePath    string
	BranchName      string
}

var activeWorktree WorktreeState

func makeStartWorktreeTool(sdPtr *string, worktreeBase string) Tool {
	return Tool{
		Name:        "start_worktree",
		Description: "Create a git worktree at a temporary location and set the agent's working directory to it. All subsequent file/git/bash tools operate within this worktree until stop_worktree is called. Multiple agents can each have their own active worktree in parallel. Each agent must use a unique branch name — if the branch is already checked out in another worktree, the tool will fail with a clear error. Uses git CLI for worktree operations.",
		Permission:  PermissionInternal,
		TimeoutSec:  30,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"branch": map[string]any{
					"type":        "string",
					"description": "Branch name to create and check out in the worktree. If the branch doesn't exist, it is created from HEAD.",
				},
			},
			"required": []string{"branch"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			branch, _ := args["branch"].(string)
			if branch == "" {
				return "", &ToolError{Message: "branch is required"}
			}

			activeWorktree.mu.Lock()
			defer activeWorktree.mu.Unlock()

			if activeWorktree.Active {
				return "", &ToolError{Message: "a worktree is already active; call stop_worktree first"}
			}

			// Determine the repository root from current safeDir
			repoRoot, err := gitRoot(*sdPtr)
			if err != nil {
				return "", &ToolError{Message: "current directory is not inside a git repository"}
			}

			// Create worktree base directory if it doesn't exist
			if worktreeBase == "" {
				worktreeBase = "/tmp/cima"
			}
			if err := os.MkdirAll(worktreeBase, 0755); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("failed to create worktree base: %s", err)}
			}

			// Generate a unique worktree path
			safeName := sanitizeBranchName(branch)
			worktreePath := filepath.Join(worktreeBase, safeName)

			// Check if the worktree path already exists
			if _, err := os.Stat(worktreePath); err == nil {
				return "", &ToolError{Message: fmt.Sprintf("worktree path already exists: %s", worktreePath)}
			}

			// Build the git worktree add command
			// Try creating the branch from HEAD first
			argsList := []string{"worktree", "add", worktreePath}
			argsList = append(argsList, "-b", branch)

			cmd := execCommand("git", argsList...)
			cmd.Dir = repoRoot
			output, err := cmd.CombinedOutput()
			if err != nil {
				// If branch already exists, try checking it out without -b
				if strings.Contains(string(output), "already exists") {
					cmd = execCommand("git", "worktree", "add", worktreePath, branch)
					cmd.Dir = repoRoot
					output, err = cmd.CombinedOutput()
				}
				if err != nil {
					return "", &ToolError{Message: fmt.Sprintf("git worktree add failed: %s", strings.TrimSpace(string(output)))}
				}
			}

			activeWorktree.OriginalSafeDir = *sdPtr
			activeWorktree.WorktreePath = worktreePath
			activeWorktree.BranchName = branch
			activeWorktree.Active = true

			// Update safeDir to point to the worktree
			*sdPtr = worktreePath

			return fmt.Sprintf("ok (worktree created at %s on branch %s)", worktreePath, branch), nil
		},
	}
}

func makeStopWorktreeTool(sdPtr *string, originalSafeDir *string) Tool {
	return Tool{
		Name:        "stop_worktree",
		Description: "Stop the current worktree session and return to the main repository. Cleans up the worktree directory, git worktree metadata, and deletes the worktree branch. After calling this, all tools operate on the original repository again.\nRequires `force: true` if the worktree has uncommitted changes, or if the branch has commits not yet merged into HEAD.\nUse `force: true` to discard uncommitted changes and delete the branch.",
		Permission:  PermissionInternal,
		TimeoutSec:  30,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"force": map[string]any{
					"type":        "boolean",
					"description": "If true, discard uncommitted changes and delete the branch even if it is dirty or unmerged. Default false.",
				},
			},
			"required": []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			force := false
			if v, ok := args["force"].(bool); ok {
				force = v
			}

			activeWorktree.mu.Lock()
			defer activeWorktree.mu.Unlock()

			if !activeWorktree.Active {
				return "", &ToolError{Message: "no active worktree to stop"}
			}

			// Prune stale worktree metadata first
			pruneCmd := execCommand("git", "worktree", "prune")
			pruneCmd.Dir = activeWorktree.OriginalSafeDir
			pruneCmd.Run()

			// Remove the worktree using git worktree remove
			removeArgs := []string{"worktree", "remove"}
			if force {
				removeArgs = append(removeArgs, "--force")
			}
			removeArgs = append(removeArgs, activeWorktree.WorktreePath)

			cmd := execCommand("git", removeArgs...)
			cmd.Dir = activeWorktree.OriginalSafeDir
			output, err := cmd.CombinedOutput()
			if err != nil {
				// If git worktree remove failed, fall back to manual cleanup
				if !force {
					return "", &ToolError{Message: fmt.Sprintf("git worktree remove failed (use force=true to discard): %s", strings.TrimSpace(string(output)))}
				}
				// Force: manually remove the worktree directory
				removeAllSafe(activeWorktree.WorktreePath)
			}

			// Restore safeDir to original
			*sdPtr = activeWorktree.OriginalSafeDir
			if originalSafeDir != nil {
				*originalSafeDir = activeWorktree.OriginalSafeDir
			}

			// Reset state
			info := fmt.Sprintf("ok (worktree %s on branch %s removed, returned to %s)",
				activeWorktree.WorktreePath, activeWorktree.BranchName, activeWorktree.OriginalSafeDir)
			activeWorktree.Active = false
			activeWorktree.WorktreePath = ""
			activeWorktree.BranchName = ""
			activeWorktree.OriginalSafeDir = ""

			return info, nil
		},
	}
}

// sanitizeBranchName converts a branch name to a safe filesystem component.
func sanitizeBranchName(branch string) string {
	var sb strings.Builder
	for _, c := range branch {
		if c == '/' || c == '\\' || c == '\x00' || c == '.' || c == ' ' {
			sb.WriteByte('-')
		} else {
			sb.WriteRune(c)
		}
	}
	return sb.String()
}

// removeAllSafe recursively deletes a directory without following symlinks.
func removeAllSafe(dir string) {
	filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		if info.Mode()&os.ModeSymlink != 0 {
			os.Remove(path)
			return filepath.SkipDir
		}
		return nil
	})
	os.RemoveAll(dir)
}

// CleanupWorktree safely removes any active worktree directory.
// Called on application shutdown to ensure no stale directories remain.
// This is a best-effort cleanup; errors are silently ignored.
func CleanupWorktree() {
	activeWorktree.mu.Lock()
	defer activeWorktree.mu.Unlock()

	if !activeWorktree.Active {
		return
	}

	// Try git worktree remove first
	pruneCmd := execCommand("git", "worktree", "prune")
	_ = pruneCmd.Run()

	removeArgs := []string{"worktree", "remove", "--force", activeWorktree.WorktreePath}
	cmd := execCommand("git", removeArgs...)
	_ = cmd.Run()

	// Fall back to manual removal
	if activeWorktree.WorktreePath != "" {
		removeAllSafe(activeWorktree.WorktreePath)
	}

	activeWorktree.Active = false
	activeWorktree.WorktreePath = ""
	activeWorktree.BranchName = ""
	activeWorktree.OriginalSafeDir = ""
}
