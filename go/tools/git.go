package tools

import (
	"bytes"
	"context"
	"fmt"
	"os/exec"
	"sort"
	"strings"
	"sync"
	"time"

	gogit "github.com/go-git/go-git/v5"
	"github.com/go-git/go-git/v5/plumbing"
	"github.com/go-git/go-git/v5/plumbing/object"
)

// Global mutex to serialize git index write operations
var gitIndexMutex sync.Mutex

func makeGitStatusTool(sdPtr *string) Tool {
	return Tool{
		Name:        "git_status",
		Description: "Return the working tree status in short format (like 'git status --short').\nEach line uses the two-character porcelain format:\n  XY <path>\nwhere X is the index status and Y is the working tree status.\n  ' ' = unmodified, M = modified, A = added, D = deleted, R = renamed, C = copied, U = updated, ? = untracked, ! = ignored\nOutput is sorted by path and capped at 200 entries.",
		Permission:  PermissionReadOnly,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type":       "object",
			"properties": map[string]any{},
			"required":   []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			repo, err := ensureRepoOpen(*sdPtr)
			if err != nil {
				return "", &ToolError{Message: err.Error()}
			}

			wt, err := repo.Worktree()
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git worktree error: %s", err)}
			}

			status, err := wt.Status()
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git status error: %s", err)}
			}

			type entry struct {
				path string
				x    byte
				y    byte
			}
			var entries []entry
			const maxEntries = 200

			for path := range status {
				if len(entries) >= maxEntries {
					break
				}
				fs := status[path]
				e := entry{
					path: path,
					x:    gitStatusCodeChar(fs.Staging),
					y:    gitStatusCodeChar(fs.Worktree),
				}
				entries = append(entries, e)
			}

			sort.Slice(entries, func(i, j int) bool {
				return entries[i].path < entries[j].path
			})

			var result strings.Builder
			for _, e := range entries {
				result.WriteByte(e.x)
				result.WriteByte(e.y)
				result.WriteByte(' ')
				result.WriteString(e.path)
				result.WriteByte('\n')
			}

			if result.Len() == 0 {
				return "(clean -- no changes)", nil
			}
			return result.String(), nil
		},
	}
}

func makeGitDiffTool(sdPtr *string) Tool {
	return Tool{
		Name:        "git_diff",
		Description: "Return a unified diff of unstaged (or staged) changes.\nOutput is capped at 500 lines / 16000 chars.\nUse git_status first to see which files have changed, then git_diff to inspect the actual changes.\nIf 'staged' is true, shows the diff that would be committed (index vs HEAD). If false (default), shows unstaged changes (working tree vs index).",
		Permission:  PermissionReadOnly,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"staged": map[string]any{
					"type":        "boolean",
					"description": "If true, show staged changes (diff of index vs HEAD). If false (default), show unstaged changes (diff of working tree vs index).",
				},
				"path": map[string]any{
					"type":        "string",
					"description": "Optional file path to limit the diff to a specific file. If provided, only this file's changes are shown. Must be under the safe directory.",
				},
			},
			"required": []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			staged := false
			if v, ok := args["staged"].(bool); ok {
				staged = v
			}
			filterPath, _ := args["path"].(string)

			// Build git diff command
			gitArgs := []string{"diff", "--no-color"}
			if staged {
				gitArgs = append(gitArgs, "--staged")
			}
			if filterPath != "" {
				// Resolve path for safety, then use relative path
				resolved, err := ResolvePath(filterPath, *sdPtr)
				if err != nil {
					return "", err
				}
				// Get relative path from safeDir
				rel, err := filepathRel(*sdPtr, resolved)
				if err == nil {
					gitArgs = append(gitArgs, "--", rel)
				}
			}

			ctx, cancel := context.WithTimeout(ctx, 10*time.Second)
			defer cancel()

			cmd := exec.CommandContext(ctx, "git", gitArgs...)
			cmd.Dir = *sdPtr
			var stdout, stderr bytes.Buffer
			cmd.Stdout = &stdout
			cmd.Stderr = &stderr

			err := cmd.Run()
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git diff error: %s", strings.TrimSpace(stderr.String()))}
			}

			output := stdout.String()

			// Cap output
			lines := strings.Split(output, "\n")
			if len(lines) > 500 {
				lines = lines[:500]
				output = strings.Join(lines, "\n") + "\n...(diff truncated at 500 lines)"
			}
			if len(output) > 16000 {
				output = output[:16000] + "...(diff truncated at 16000 chars)"
			}

			if output == "" {
				if staged {
					return "(no staged changes)", nil
				}
				return "(no unstaged changes)", nil
			}
			return output, nil
		},
	}
}

func makeGitLogTool(sdPtr *string) Tool {
	return Tool{
		Name:        "git_log",
		Description: "Return recent commit history.\nOutput formats:\n  'short' (default): commit hash, author, date, subject\n  'oneline': <hash_prefix> <subject>\n  'full': commit hash, author, date, and full message body\nUse 'branch' to specify a revision (branch, tag, commit hash, HEAD~N, etc.).\nDefaults to HEAD (current branch tip).",
		Permission:  PermissionReadOnly,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"max_count": map[string]any{
					"type":        "integer",
					"description": "Maximum number of commits to return (default 10, max 50)",
				},
				"format": map[string]any{
					"type":        "string",
					"enum":        []string{"oneline", "short", "full"},
					"description": "Output format: oneline, short (default), or full",
				},
				"branch": map[string]any{
					"type":        "string",
					"description": "Git revision to start from (e.g. 'main', 'HEAD~3', 'v1.0'). Defaults to HEAD.",
				},
			},
			"required": []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			repo, err := ensureRepoOpen(*sdPtr)
			if err != nil {
				return "", &ToolError{Message: err.Error()}
			}

			maxCount := 10
			if v := getIntArg(args, "max_count"); v > 0 {
				maxCount = int(v)
				if maxCount < 1 {
					maxCount = 1
				}
				if maxCount > 50 {
					maxCount = 50
				}
			}

			format := "short"
			if v, ok := args["format"].(string); ok {
				format = v
			}
			if format != "oneline" && format != "short" && format != "full" {
				return "", &ToolError{Message: fmt.Sprintf("invalid format '%s'. Must be 'oneline', 'short', or 'full'.", format)}
			}

			// Get log options
			opts := &gogit.LogOptions{All: false}

			if branch, ok := args["branch"].(string); ok && branch != "" {
				// Try to resolve the branch reference
				ref, err := repo.Reference(plumbing.ReferenceName("refs/heads/"+branch), true)
				if err != nil {
					// Try as a tag or hash
					hash, err := repo.ResolveRevision(plumbing.Revision(branch))
					if err != nil {
						return "", &ToolError{Message: fmt.Sprintf("git_log error: cannot resolve '%s'", branch)}
					}
					opts.From = *hash
				} else {
					opts.From = ref.Hash()
				}
			}

			iter, err := repo.Log(opts)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git_log error: %s", err)}
			}

			var result strings.Builder
			count := 0
			const maxChars = 16000

			err = iter.ForEach(func(c *object.Commit) error {
				if count >= maxCount || result.Len() >= maxChars {
					return fmt.Errorf("stop")
				}
				count++

				hash := c.Hash.String()[:8]
				author := c.Author.Name + " <" + c.Author.Email + ">"
				date := c.Author.When.Format("2006-01-02 15:04:05")
				msg := strings.TrimSpace(c.Message)
				msgFirstLine := msg
				if idx := strings.Index(msg, "\n"); idx >= 0 {
					msgFirstLine = msg[:idx]
				}

				switch format {
				case "oneline":
					result.WriteString(fmt.Sprintf("%s %s\n", hash, msgFirstLine))
				case "full":
					result.WriteString(fmt.Sprintf("commit %s\n", c.Hash.String()))
					result.WriteString(fmt.Sprintf("Author: %s\n", author))
					result.WriteString(fmt.Sprintf("Date:   %s\n", date))
					result.WriteString("\n")
					for _, l := range strings.Split(msg, "\n") {
						result.WriteString(fmt.Sprintf("    %s\n", l))
					}
					result.WriteString("\n")
				default:
					result.WriteString(fmt.Sprintf("%s  %s  %s  %s\n", hash, author, date, msgFirstLine))
				}
				return nil
			})

			if result.Len() == 0 {
				return "(no commits found)", nil
			}
			return result.String(), nil
		},
	}
}

func makeGitAddTool(sdPtr *string) Tool {
	return Tool{
		Name:        "git_add",
		Description: "Stage file(s) for commit. Like 'git add <path>' or 'git add -A'. If 'all' is true, stages all changes (added, modified, deleted) in the entire working tree. If 'path' is specified, only that file or pathspec is staged. Use git_status first to see which files are changed, then git_add to stage them, then git_commit to commit.",
		Permission:  PermissionWrite,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"all": map[string]any{
					"type":        "boolean",
					"description": "If true, stage all changes. Like 'git add -A'. Default false.",
				},
				"path": map[string]any{
					"type":        "string",
					"description": "File path or pathspec to stage. Must be under the safe directory.",
				},
			},
			"required": []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			addAll := false
			if v, ok := args["all"].(bool); ok {
				addAll = v
			}
			pathStr, _ := args["path"].(string)

			gitIndexMutex.Lock()
			defer gitIndexMutex.Unlock()

			repo, err := ensureRepoOpen(*sdPtr)
			if err != nil {
				return "", &ToolError{Message: err.Error()}
			}

			wt, err := repo.Worktree()
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git add error: %s", err)}
			}

			if addAll {
				err = wt.AddWithOptions(&gogit.AddOptions{All: true})
				if err != nil {
					return "", &ToolError{Message: fmt.Sprintf("git add -A error: %s", err)}
				}
				return "ok (staged all changes)", nil
			}

			if pathStr == "" {
				return "", &ToolError{Message: "either 'path' or 'all' must be specified"}
			}

			_, err = wt.Add(pathStr)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git add error: %s", err)}
			}

			return fmt.Sprintf("ok (staged %s)", pathStr), nil
		},
	}
}

func makeGitCommitTool(sdPtr *string) Tool {
	return Tool{
		Name:        "git_commit",
		Description: "Create a new commit with staged changes. Like 'git commit -m <message>'. If 'all' is true, stages all changes before committing (like 'git commit -a'). Use git_status first to see which files are changed, then git_add to stage them, then git_commit to commit.",
		Permission:  PermissionWrite,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"message": map[string]any{
					"type":        "string",
					"description": "Commit message.",
				},
				"all": map[string]any{
					"type":        "boolean",
					"description": "If true, stage all changes before committing. Like 'git commit -a'. Default false.",
				},
			},
			"required": []string{"message"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			message, _ := args["message"].(string)
			if message == "" {
				return "", &ToolError{Message: "message is required"}
			}

			commitAll := false
			if v, ok := args["all"].(bool); ok {
				commitAll = v
			}

			gitIndexMutex.Lock()
			defer gitIndexMutex.Unlock()

			repo, err := ensureRepoOpen(*sdPtr)
			if err != nil {
				return "", &ToolError{Message: err.Error()}
			}

			wt, err := repo.Worktree()
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git commit error: %s", err)}
			}

			if commitAll {
				err = wt.AddWithOptions(&gogit.AddOptions{All: true})
				if err != nil {
					return "", &ToolError{Message: fmt.Sprintf("git add -A error: %s", err)}
				}
			}

			commit, err := wt.Commit(message, &gogit.CommitOptions{
				Author: &object.Signature{
					Name:  "cima",
					Email: "cima@agent",
					When:  time.Now(),
				},
			})
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("git commit error: %s", err)}
			}

			return fmt.Sprintf("ok (committed %s)", commit.String()[:8]), nil
		},
	}
}

// gitStatusCodeChar maps a go-git StatusCode to a porcelain v1 character.
func gitStatusCodeChar(code gogit.StatusCode) byte {
	switch code {
	case gogit.Unmodified:
		return ' '
	case gogit.Modified:
		return 'M'
	case gogit.Added:
		return 'A'
	case gogit.Deleted:
		return 'D'
	case gogit.Renamed:
		return 'R'
	case gogit.Copied:
		return 'C'
	case gogit.UpdatedButUnmerged:
		return 'U'
	case gogit.Untracked:
		return '?'
	default:
		return ' '
	}
}

// filepathRel is a helper to get relative path.
func filepathRel(base, target string) (string, error) {
	return strings.TrimPrefix(target, base+"/"), nil
}
