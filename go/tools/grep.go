package tools

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

func makeGrepFilesTool(sdPtr *string, readOnlyPaths []string) Tool {
	return Tool{
		Name:        "grep_files",
		Description: "Search file contents using a regex pattern (max 200 results)",
		Permission:  PermissionReadOnly,
		TimeoutSec:  10,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"pattern": map[string]any{
					"type":        "string",
					"description": "Regex pattern to search for",
				},
				"path": map[string]any{
					"type":        "string",
					"description": "File or directory to search in (defaults to .)",
				},
			},
			"required": []string{"pattern"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			pattern, _ := args["pattern"].(string)
			if pattern == "" {
				return "", &ToolError{Message: "pattern is required"}
			}

			re, err := regexp.Compile(pattern)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("invalid regex: %s", err)}
			}

			rawPath, _ := args["path"].(string)
			if rawPath == "" {
				rawPath = "."
			}
			resolved, err := ResolvePath(rawPath, *sdPtr, readOnlyPaths...)
			if err != nil {
				return "", err
			}

			var result strings.Builder
			count := 0
			maxResults := 200

			var searchFile func(path string)
			searchFile = func(path string) {
				if count >= maxResults {
					return
				}
				file, err := os.Open(path)
				if err != nil {
					return
				}
				defer file.Close()

				scanner := bufio.NewScanner(file)
				lineNum := 0
				for scanner.Scan() && count < maxResults {
					lineNum++
					line := scanner.Text()
					if re.MatchString(line) {
						result.WriteString(fmt.Sprintf("%s:%d: %s\n", path, lineNum, line))
						count++
					}
				}
			}

			info, err := os.Stat(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Cannot access path: %s", resolved)}
			}

			if !info.IsDir() {
				searchFile(resolved)
			} else {
				err = filepath.Walk(resolved, func(path string, info os.FileInfo, err error) error {
					if err != nil {
						return nil // skip permission errors
					}
					if info.IsDir() && info.Name() == ".git" {
						return filepath.SkipDir
					}
					if !info.IsDir() && count < maxResults {
						searchFile(path)
					}
					return nil
				})
				if err != nil {
					return "", &ToolError{Message: fmt.Sprintf("Error walking directory: %s", err)}
				}
			}

			if result.Len() == 0 {
				return "(no matches)", nil
			}
			return result.String(), nil
		},
	}
}
