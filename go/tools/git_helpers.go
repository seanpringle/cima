package tools

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	gogit "github.com/go-git/go-git/v5"
)

// execCommand is a variable so tests can replace it with a mock.
var execCommand = exec.Command

// OpenGitRepo opens the git repository at or walking up from path.
func OpenGitRepo(path string) (*gogit.Repository, string, error) {
	repo, err := gogit.PlainOpenWithOptions(path, &gogit.PlainOpenOptions{
		DetectDotGit: true,
	})
	if err != nil {
		return nil, "", fmt.Errorf("not a git repository: %s", path)
	}
	return repo, path, nil
}

// GetCurrentGitBranch returns the current branch name at the given repository path.
func GetCurrentGitBranch(repoPath string) (string, error) {
	repo, _, err := OpenGitRepo(repoPath)
	if err != nil {
		return "", err
	}

	ref, err := repo.Head()
	if err != nil {
		return "", fmt.Errorf("failed to get HEAD: %s", err)
	}

	if ref.Name().IsBranch() {
		return ref.Name().Short(), nil
	}

	// Detached HEAD
	hash := ref.Hash().String()
	if len(hash) > 7 {
		hash = hash[:7]
	}
	return "(detached HEAD at " + hash + ")", nil
}

// IsGitignored checks if a path within a repo is ignored by .gitignore rules.
// Uses `git check-ignore` via CLI since go-git doesn't expose gitignore checking directly.
func IsGitignored(repo *gogit.Repository, absPath string) bool {
	if repo == nil {
		return false
	}

	wt, err := repo.Worktree()
	if err != nil {
		return false
	}

	wtDir := wt.Filesystem.Root()

	// Use git check-ignore to determine if the path is ignored
	cmd := execCommand("git", "-C", wtDir, "check-ignore", "-q", absPath)
	err = cmd.Run()
	return err == nil // exit 0 = ignored, non-zero = not ignored
}

// ensureRepoOpen opens a repo from safeDir. Returns nil if not a git repo.
func ensureRepoOpen(safeDir string) (*gogit.Repository, error) {
	repo, _, err := OpenGitRepo(safeDir)
	return repo, err
}

// worktreeDir returns the worktree directory (i.e., the root of the repo).
func worktreeDir(repo *gogit.Repository) (string, error) {
	wt, err := repo.Worktree()
	if err != nil {
		return "", err
	}
	abs, err := filepath.Abs(wt.Filesystem.Root())
	if err != nil {
		return "", err
	}
	return abs, nil
}

// gitRoot returns the root directory of the git repository containing path.
func gitRoot(path string) (string, error) {
	repo, err := ensureRepoOpen(path)
	if err != nil {
		return "", err
	}
	return worktreeDir(repo)
}

// fileExists checks if a file exists.
func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
