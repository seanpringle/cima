package tools

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

func makeListFilesTool(sdPtr *string, readOnlyPaths []string) Tool {
	return Tool{
		Name:        "list_files",
		Description: "List files and directories in a given path",
		Permission:  PermissionReadOnly,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Directory path to list",
				},
			},
			"required": []string{"path"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			resolved, err := ResolvePath(raw, *sdPtr, readOnlyPaths...)
			if err != nil {
				return "", err
			}

			info, err := os.Stat(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Cannot access path: %s", resolved)}
			}
			if !info.IsDir() {
				return "", &ToolError{Message: fmt.Sprintf("Not a directory: %s", resolved)}
			}

			entries, err := os.ReadDir(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Cannot list directory: %s", resolved)}
			}

			var result strings.Builder
			for _, entry := range entries {
				prefix := "-"
				if entry.IsDir() {
					prefix = "d"
				}
				result.WriteString(prefix)
				result.WriteString(" ")
				result.WriteString(entry.Name())
				result.WriteString("\n")
			}
			if result.Len() == 0 {
				return "(empty directory)", nil
			}
			return result.String(), nil
		},
	}
}

func makeProjectTreeTool(sdPtr *string, readOnlyPaths []string) Tool {
	return Tool{
		Name:        "project_tree",
		Description: "Recursively list files/directories in a tree-like format.\nMaximum depth of 5 to avoid huge outputs. Use this to understand project structure in a single call instead of calling list_files repeatedly.",
		Permission:  PermissionReadOnly,
		TimeoutSec:  5,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Starting directory path (default '.')",
				},
				"max_depth": map[string]any{
					"type":        "integer",
					"description": "Maximum recursion depth (default 5, max 10)",
				},
				"max_lines": map[string]any{
					"type":        "integer",
					"description": "Maximum output lines (default 500, max 500)",
				},
			},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			if raw == "" {
				raw = "."
			}
			resolved, err := ResolvePath(raw, *sdPtr, readOnlyPaths...)
			if err != nil {
				return "", err
			}

			maxDepth := 5
			if v := getIntArg(args, "max_depth"); v > 0 {
				maxDepth = v
				if maxDepth < 1 {
					maxDepth = 1
				}
				if maxDepth > 10 {
					maxDepth = 10
				}
			}

			maxLines := 500
			if v := getIntArg(args, "max_lines"); v > 0 {
				maxLines = v
				if maxLines < 1 {
					maxLines = 1
				}
				if maxLines > 500 {
					maxLines = 500
				}
			}

			info, err := os.Stat(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Cannot access path: %s", resolved)}
			}
			if !info.IsDir() {
				return resolved + "\n", nil
			}

			var result strings.Builder
			lineCount := 0
			truncated := false

			var walk func(dir string, depth int, prefix string)
			walk = func(dir string, depth int, prefix string) {
				if depth >= maxDepth || lineCount >= maxLines {
					return
				}

				entries, err := os.ReadDir(dir)
				if err != nil {
					return
				}

				// Sort: directories first, then files; alphabetically within each group
				sort.Slice(entries, func(i, j int) bool {
					if entries[i].IsDir() != entries[j].IsDir() {
						return entries[i].IsDir()
					}
					return strings.ToLower(entries[i].Name()) < strings.ToLower(entries[j].Name())
				})

				// Filter out .git entries
			var displayEntries []os.DirEntry
			for _, e := range entries {
				if e.Name() == ".git" {
					continue
				}
				displayEntries = append(displayEntries, e)
			}
			entries = displayEntries

			for i, entry := range entries {
				if lineCount >= maxLines {
					truncated = true
					return
				}

				isLast := i == len(entries)-1
				connector := "├── "
				if isLast {
					connector = "└── "
				}

				result.WriteString(prefix + connector + entry.Name())
				if entry.IsDir() {
					result.WriteString("/")
				}
				result.WriteString("\n")
				lineCount++

				if entry.IsDir() && depth < maxDepth {
						childPrefix := prefix
						if isLast {
							childPrefix += "    "
						} else {
							childPrefix += "│   "
						}
						walk(filepath.Join(dir, entry.Name()), depth+1, childPrefix)
					}
				}
			}

			// Root line
			result.WriteString(resolved + "/\n")
			lineCount++

			walk(resolved, 1, "")

			if truncated {
				result.WriteString(fmt.Sprintf("...(truncated, >%d lines)\n", maxLines))
			} else if lineCount <= 1 {
				// Check if directory is actually empty (excluding .git)
				entries, _ := os.ReadDir(resolved)
				visible := 0
				for _, e := range entries {
					if e.Name() != ".git" {
						visible++
					}
				}
				if visible == 0 {
					result.WriteString("(empty directory)\n")
				}
			}

			return result.String(), nil
		},
	}
}
