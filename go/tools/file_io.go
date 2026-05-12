package tools

import (
	"bufio"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
)

func makeReadFileTool(sdPtr *string, readOnlyPaths []string) Tool {
	return Tool{
		Name:        "read_file",
		Description: "Read lines from a file (max 400 lines at a time, use offset to paginate)",
		Permission:  PermissionReadOnly,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Path to the file to read",
				},
				"offset": map[string]any{
					"type":        "integer",
					"description": "Line number to start from (1-indexed, default 0 = beginning)",
				},
				"max_lines": map[string]any{
					"type":        "integer",
					"description": "Maximum lines to read starting from offset (default 200)",
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

			offset := 0
			if v := getIntArg(args, "offset"); v >= 0 {
				offset = int(v)
				if offset < 0 {
					offset = 0
				}
			}
			maxLines := 200
			if v := getIntArg(args, "max_lines"); v > 0 {
				maxLines = int(v)
				if maxLines < 1 {
					maxLines = 1
				}
			}

			file, err := os.Open(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to open file: %s", resolved)}
			}
			defer file.Close()

			var result strings.Builder
			scanner := bufio.NewScanner(file)
			lineNum := 0
			count := 0

			// Skip lines before offset
			for lineNum < offset && scanner.Scan() {
				lineNum++
			}

			for scanner.Scan() && count < maxLines {
				lineNum++
				result.WriteString(scanner.Text())
				result.WriteString("\n")
				count++
			}

			// Check for more lines
			hasMore := false
			if scanner.Scan() {
				hasMore = true
			}

			if hasMore {
				result.WriteString(fmt.Sprintf("...(truncated, >%d lines from offset %d)", maxLines, offset))
			} else if count == 0 && offset > 0 {
				result.WriteString(fmt.Sprintf("(offset %d is beyond end of file)", offset))
			}

			return result.String(), nil
		},
	}
}

func makeReadFileLinesTool(sdPtr *string, readOnlyPaths []string) Tool {
	return Tool{
		Name:        "read_file_lines",
		Description: "Read specific line ranges from a file. Returns lines prefixed with line numbers. Use this when you know the line numbers you want (e.g. after a grep match at line 52, read lines 45-78). For reading from an offset, use read_file instead.",
		Permission:  PermissionReadOnly,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Path to the file",
				},
				"start_line": map[string]any{
					"type":        "integer",
					"description": "First line to read (1-indexed, default 1)",
				},
				"end_line": map[string]any{
					"type":        "integer",
					"description": "Last line to read (inclusive). If omitted, reads to end of file (capped by max_lines).",
				},
				"max_lines": map[string]any{
					"type":        "integer",
					"description": "Maximum lines to return (default 200, max 500)",
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

			startLine := 1
			if v := getIntArg(args, "start_line"); v > 0 {
				startLine = int(v)
				if startLine < 1 {
					startLine = 1
				}
			}
			endLine := 0
			if v := getIntArg(args, "end_line"); v >= 0 {
				endLine = int(v)
			}
			maxLines := 200
			if v := getIntArg(args, "max_lines"); v > 0 {
				maxLines = int(v)
				if maxLines < 1 {
					maxLines = 1
				}
				if maxLines > 500 {
					maxLines = 500
				}
			}

			if endLine != 0 && endLine < startLine {
				return "", &ToolError{Message: "end_line must be >= start_line"}
			}

			file, err := os.Open(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to open file: %s", resolved)}
			}
			defer file.Close()

			var result strings.Builder
			scanner := bufio.NewScanner(file)
			lineNum := 0
			count := 0

			// Skip lines before startLine
			for lineNum < startLine-1 && scanner.Scan() {
				lineNum++
			}

			maxToRead := maxLines
			if endLine != 0 {
				range_ := endLine - startLine + 1
				if range_ < maxToRead {
					maxToRead = range_
				}
			}

			for count < maxToRead && scanner.Scan() {
				lineNum++
				result.WriteString(fmt.Sprintf("%d: %s\n", lineNum, scanner.Text()))
				count++
			}

			// Detect if there are more lines
			hasMore := false
			remaining := 0
			if endLine != 0 && lineNum < endLine {
				hasMore = true
				for scanner.Scan() {
					remaining++
				}
			} else if scanner.Scan() {
				hasMore = true
				remaining = 1
				for scanner.Scan() {
					remaining++
				}
			}

			if hasMore {
				result.WriteString(fmt.Sprintf("...(truncated, >%d lines from line %d)", count+remaining, startLine))
			} else if count == 0 && startLine > 1 {
				result.WriteString(fmt.Sprintf("(start_line %d is beyond end of file)", startLine))
			}

			return result.String(), nil
		},
	}
}

func makeWriteFileTool(sdPtr *string) Tool {
	return Tool{
		Name:        "write_file",
		Description: "Write content to a file, creating parent directories if needed",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "File path",
				},
				"content": map[string]any{
					"type":        "string",
					"description": "Content to write",
				},
			},
			"required": []string{"path", "content"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			content, _ := args["content"].(string)

			resolved, err := ResolvePath(raw, *sdPtr)
			if err != nil {
				return "", err
			}

			// Create parent directories
			parent := filepath.Dir(resolved)
			if err := os.MkdirAll(parent, 0755); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to create parent directories: %s", err)}
			}

			if err := os.WriteFile(resolved, []byte(content), 0644); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to write file: %s", err)}
			}

			return fmt.Sprintf("ok (%d bytes written)", len(content)), nil
		},
	}
}

func makeEditFileTool(sdPtr *string) Tool {
	return Tool{
		Name:        "edit_file",
		Description: "Edit a file by searching for an exact string and replacing it. The search string must match exactly once in the file — this ensures edits are safe and unambiguous. Use this to make targeted surgical edits instead of rewriting entire files with write_file.",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "File path to edit",
				},
				"search": map[string]any{
					"type":        "string",
					"description": "Exact string to search for; must match exactly once in the file. Include surrounding context (unique nearby lines) to guarantee a single match.",
				},
				"replace": map[string]any{
					"type":        "string",
					"description": "String to replace the matched occurrence with",
				},
			},
			"required": []string{"path", "search", "replace"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			search, _ := args["search"].(string)
			replace, _ := args["replace"].(string)

			if search == "" {
				return "", &ToolError{Message: "search string is required"}
			}

			resolved, err := ResolvePath(raw, *sdPtr)
			if err != nil {
				return "", err
			}

			// Read file
			data, err := os.ReadFile(resolved)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to read file: %s", resolved)}
			}
			content := string(data)

			// Count occurrences
			count := strings.Count(content, search)
			if count == 0 {
				return "", &ToolError{Message: "Search string not found in file (0 matches). Use read_file or grep_files to verify the file contents."}
			}
			if count > 1 {
				return "", &ToolError{Message: fmt.Sprintf("Search string found %d times in file (expected exactly 1). Include more surrounding context in the search string to uniquely identify the location.", count)}
			}

			// Find position for line number computation
			pos := strings.Index(content, search)
			if pos < 0 {
				return "", &ToolError{Message: "unexpected error: search string not found after count check"}
			}

			// Replace
			newContent := strings.Replace(content, search, replace, 1)

			// Write back
			if err := os.WriteFile(resolved, []byte(newContent), 0644); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to write file: %s", err)}
			}

			// Compute line number
			lineNum := 1
			for i := 0; i < pos; i++ {
				if content[i] == '\n' {
					lineNum++
				}
			}

			return fmt.Sprintf("ok (replaced 1 occurrence at line %d, %d bytes -> %d bytes)", lineNum, len(search), len(replace)), nil
		},
	}
}

func makeDeleteFileTool(sdPtr *string) Tool {
	return Tool{
		Name:        "delete_file",
		Description: "Delete a file",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "File path to delete",
				},
			},
			"required": []string{"path"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			resolved, err := ResolvePath(raw, *sdPtr)
			if err != nil {
				return "", err
			}

			info, err := os.Stat(resolved)
			if err != nil {
				if os.IsNotExist(err) {
					return "", &ToolError{Message: fmt.Sprintf("File not found: %s", resolved)}
				}
				return "", &ToolError{Message: fmt.Sprintf("Cannot access file: %s", resolved)}
			}
			if info.IsDir() {
				return "", &ToolError{Message: fmt.Sprintf("Not a regular file: %s", resolved)}
			}

			size := info.Size()
			if err := os.Remove(resolved); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to delete file: %s", err)}
			}

			return fmt.Sprintf("ok (deleted %s, %d bytes)", resolved, size), nil
		},
	}
}

func makeMoveFileTool(sdPtr *string) Tool {
	return Tool{
		Name:        "move_file",
		Description: "Move or rename a file from source to destination. Works for both same-directory renames and cross-directory moves. Will not overwrite an existing destination.",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"source": map[string]any{
					"type":        "string",
					"description": "Source file path",
				},
				"destination": map[string]any{
					"type":        "string",
					"description": "Destination file path",
				},
			},
			"required": []string{"source", "destination"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			srcRaw, _ := args["source"].(string)
			dstRaw, _ := args["destination"].(string)

			if srcRaw == "" || dstRaw == "" {
				return "", &ToolError{Message: "source and destination are required"}
			}

			src, err := ResolvePath(srcRaw, *sdPtr)
			if err != nil {
				return "", err
			}
			dst, err := ResolvePath(dstRaw, *sdPtr)
			if err != nil {
				return "", err
			}

			if _, err := os.Stat(src); os.IsNotExist(err) {
				return "", &ToolError{Message: fmt.Sprintf("Source not found: %s", src)}
			}
			if _, err := os.Stat(dst); err == nil {
				return "", &ToolError{Message: fmt.Sprintf("Destination already exists: %s", dst)}
			}

			// Create parent directories of destination
			parent := filepath.Dir(dst)
			if err := os.MkdirAll(parent, 0755); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to create parent directories: %s", err)}
			}

			// Rename (may fail cross-device)
			if err := os.Rename(src, dst); err != nil {
				// Cross-device link: fall back to copy + delete
				if strings.Contains(err.Error(), "invalid cross-device link") || strings.Contains(err.Error(), "cross-device") {
					if err := copyFile(src, dst); err != nil {
						return "", &ToolError{Message: fmt.Sprintf("Failed to copy across devices: %s", err)}
					}
					if err := os.Remove(src); err != nil {
						os.Remove(dst) // clean up destination
						return "", &ToolError{Message: fmt.Sprintf("Failed to remove source after copy: %s", err)}
					}
				} else {
					return "", &ToolError{Message: fmt.Sprintf("Failed to move file: %s", err)}
				}
			}

			return fmt.Sprintf("ok (moved %s -> %s)", src, dst), nil
		},
	}
}

func makeRenameFileTool(sdPtr *string) Tool {
	return Tool{
		Name:        "rename_file",
		Description: "Rename a file within its directory. Provide the current path and the new filename (basename only, no path separators). For cross-directory moves, use move_file instead.",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"path": map[string]any{
					"type":        "string",
					"description": "Current file path",
				},
				"new_name": map[string]any{
					"type":        "string",
					"description": "New filename (basename only, no '/' or '\\' allowed)",
				},
			},
			"required": []string{"path", "new_name"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			raw, _ := args["path"].(string)
			newName, _ := args["new_name"].(string)

			if newName == "" {
				return "", &ToolError{Message: "new_name is required"}
			}
			if strings.ContainsAny(newName, "/\\") {
				return "", &ToolError{Message: "new_name must be a basename only (no path separators)"}
			}

			resolved, err := ResolvePath(raw, *sdPtr)
			if err != nil {
				return "", err
			}

			parent := filepath.Dir(resolved)
			dest := filepath.Join(parent, newName)

			if _, err := os.Stat(resolved); os.IsNotExist(err) {
				return "", &ToolError{Message: fmt.Sprintf("File not found: %s", resolved)}
			}
			if _, err := os.Stat(dest); err == nil {
				return "", &ToolError{Message: fmt.Sprintf("Destination already exists: %s", dest)}
			}

			if err := os.Rename(resolved, dest); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("Failed to rename file: %s", err)}
			}

			return fmt.Sprintf("ok (renamed %s -> %s)", resolved, dest), nil
		},
	}
}

// copyFile copies a file from src to dst (used for cross-device moves).
func copyFile(src, dst string) error {
	s, err := os.Open(src)
	if err != nil {
		return err
	}
	defer s.Close()

	d, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer d.Close()

	_, err = io.Copy(d, s)
	return err
}
