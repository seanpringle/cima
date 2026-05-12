package tools

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestListFiles(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("hello"), 0644)
	os.MkdirAll(filepath.Join(safeDir, "subdir"), 0755)

	tool := makeListFilesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "."})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "test.txt") {
		t.Errorf("result should contain test.txt: %q", result)
	}
	if !strings.Contains(result, "subdir") {
		t.Errorf("result should contain subdir: %q", result)
	}
}

func TestListFilesOutsideSafeDir(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeListFilesTool(&safeDir, nil)
	_, err := tool.Execute(context.Background(), map[string]any{"path": "/etc"})
	if err == nil {
		t.Fatal("expected error for path outside safe dir")
	}
}

func TestListFilesNotADirectory(t *testing.T) {
	safeDir := t.TempDir()
	f := filepath.Join(safeDir, "file.txt")
	os.WriteFile(f, []byte("hello"), 0644)

	tool := makeListFilesTool(&safeDir, nil)
	_, err := tool.Execute(context.Background(), map[string]any{"path": "file.txt"})
	if err == nil {
		t.Fatal("expected error for file path, not directory")
	}
}

func TestProjectTreeBasic(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "a.txt"), []byte("a"), 0644)
	os.MkdirAll(filepath.Join(safeDir, "sub"), 0755)
	os.WriteFile(filepath.Join(safeDir, "sub", "b.txt"), []byte("b"), 0644)

	tool := makeProjectTreeTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "."})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "a.txt") {
		t.Errorf("result should contain a.txt: %q", result)
	}
	if !strings.Contains(result, "sub/") {
		t.Errorf("result should contain sub/: %q", result)
	}
}

func TestProjectTreeMaxDepth(t *testing.T) {
	safeDir := t.TempDir()
	os.MkdirAll(filepath.Join(safeDir, "a", "b", "c", "d"), 0755)

	tool := makeProjectTreeTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": ".", "max_depth": 2})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	// Should show a/ but not a/b/ or deeper
	lines := strings.Split(strings.TrimSpace(result), "\n")
	for _, line := range lines {
		if strings.Contains(line, "c/") {
			t.Errorf("should not show depth 3 with max_depth=2: %s", line)
		}
	}
}

func TestProjectTreeSkipsGit(t *testing.T) {
	safeDir := t.TempDir()
	os.MkdirAll(filepath.Join(safeDir, ".git"), 0755)
	os.WriteFile(filepath.Join(safeDir, "readme.md"), []byte("hello"), 0644)

	tool := makeProjectTreeTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "."})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if strings.Contains(result, ".git") {
		t.Errorf(".git should be skipped in project tree: %q", result)
	}
}

func TestReadFile(t *testing.T) {
	safeDir := t.TempDir()
	content := "line1\nline2\nline3\n"
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte(content), 0644)

	tool := makeReadFileTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "test.txt"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "line1") {
		t.Errorf("result should contain line1: %q", result)
	}
}

func TestReadFileWithOffset(t *testing.T) {
	safeDir := t.TempDir()
	content := "line1\nline2\nline3\nline4\n"
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte(content), 0644)

	tool := makeReadFileTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "test.txt", "offset": float64(2), "max_lines": float64(2)})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if strings.Contains(result, "line1") {
		t.Errorf("result should not contain line1 (offset 2): %q", result)
	}
	if !strings.Contains(result, "line3") {
		t.Errorf("result should contain line3: %q", result)
	}
}

func TestWriteFile(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeWriteFileTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "output.txt", "content": "hello world"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "11 bytes") {
		t.Errorf("result should mention byte count: %q", result)
	}
	data, _ := os.ReadFile(filepath.Join(safeDir, "output.txt"))
	if string(data) != "hello world" {
		t.Errorf("file content = %q, want %q", string(data), "hello world")
	}
}

func TestEditFile(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("hello foo world"), 0644)

	tool := makeEditFileTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{
		"path":    "test.txt",
		"search":  "foo",
		"replace": "bar",
	})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "line 1") {
		t.Errorf("result should mention line: %q", result)
	}
	data, _ := os.ReadFile(filepath.Join(safeDir, "test.txt"))
	if string(data) != "hello bar world" {
		t.Errorf("file content = %q, want %q", string(data), "hello bar world")
	}
}

func TestEditFileNotFound(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("hello"), 0644)

	tool := makeEditFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{
		"path":    "test.txt",
		"search":  "nonexistent",
		"replace": "x",
	})
	if err == nil {
		t.Fatal("expected error for search string not found")
	}
}

func TestDeleteFile(t *testing.T) {
	safeDir := t.TempDir()
	f := filepath.Join(safeDir, "todelete.txt")
	os.WriteFile(f, []byte("hello"), 0644)

	tool := makeDeleteFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{"path": "todelete.txt"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if _, err := os.Stat(f); !os.IsNotExist(err) {
		t.Error("file should have been deleted")
	}
}

func TestRenameFile(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "old.txt"), []byte("hello"), 0644)

	tool := makeRenameFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{"path": "old.txt", "new_name": "new.txt"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if _, err := os.Stat(filepath.Join(safeDir, "old.txt")); !os.IsNotExist(err) {
		t.Error("old file should not exist after rename")
	}
	if _, err := os.Stat(filepath.Join(safeDir, "new.txt")); err != nil {
		t.Error("new file should exist after rename")
	}
}

func TestGrepFilesBasic(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("hello world\nfoo bar\n"), 0644)

	tool := makeGrepFilesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"pattern": "hello"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "hello") {
		t.Errorf("result should contain matched line: %q", result)
	}
}

func TestGrepFilesNoMatch(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("hello"), 0644)

	tool := makeGrepFilesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"pattern": "zzz"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "(no matches)" {
		t.Errorf("result = %q, want '(no matches)'", result)
	}
}

func TestRunBashEcho(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"command": "echo hello"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "hello") {
		t.Errorf("result = %q, should contain 'hello'", result)
	}
}

func TestRunBashEmptyCommand(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{"command": ""})
	if err == nil {
		t.Fatal("expected error for empty command")
	}
}

// ── read_file_lines tests ──

func TestReadFileLines(t *testing.T) {
	safeDir := t.TempDir()
	content := "line1\nline2\nline3\nline4\nline5\n"
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte(content), 0644)

	tool := makeReadFileLinesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "test.txt"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "1: line1") {
		t.Errorf("result should contain '1: line1': %q", result)
	}
	if !strings.Contains(result, "5: line5") {
		t.Errorf("result should contain '5: line5': %q", result)
	}
}

func TestReadFileLinesRange(t *testing.T) {
	safeDir := t.TempDir()
	content := "line1\nline2\nline3\nline4\nline5\n"
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte(content), 0644)

	tool := makeReadFileLinesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{
		"path":       "test.txt",
		"start_line": float64(2),
		"end_line":   float64(4),
	})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if strings.Contains(result, "1: line1") {
		t.Errorf("result should not contain line1: %q", result)
	}
	if !strings.Contains(result, "2: line2") {
		t.Errorf("result should contain '2: line2': %q", result)
	}
	if !strings.Contains(result, "4: line4") {
		t.Errorf("result should contain '4: line4': %q", result)
	}
	if strings.Contains(result, "5: line5") {
		t.Errorf("result should not contain line5: %q", result)
	}
}

// ── edit_file multiple matches test ──

func TestEditFileMultipleMatches(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "test.txt"), []byte("foo foo foo"), 0644)

	tool := makeEditFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{
		"path":    "test.txt",
		"search":  "foo",
		"replace": "bar",
	})
	if err == nil {
		t.Fatal("expected error for multiple matches")
	}
}

// ── delete_file nonexistent test ──

func TestDeleteFileNonexistent(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeDeleteFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{"path": "nonexistent.txt"})
	if err == nil {
		t.Fatal("expected error for nonexistent file")
	}
}

// ── move_file tests ──

func TestMoveFile(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "source.txt"), []byte("content"), 0644)

	tool := makeMoveFileTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{
		"source":      "source.txt",
		"destination": "dest.txt",
	})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "moved") {
		t.Errorf("result should mention 'moved': %q", result)
	}
	// Source should be gone
	if _, err := os.Stat(filepath.Join(safeDir, "source.txt")); !os.IsNotExist(err) {
		t.Error("source file should not exist after move")
	}
	// Dest should exist
	if _, err := os.Stat(filepath.Join(safeDir, "dest.txt")); err != nil {
		t.Error("destination file should exist after move")
	}
}

func TestMoveFileOverwrite(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "source.txt"), []byte("content"), 0644)
	os.WriteFile(filepath.Join(safeDir, "dest.txt"), []byte("existing"), 0644)

	tool := makeMoveFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{
		"source":      "source.txt",
		"destination": "dest.txt",
	})
	if err == nil {
		t.Fatal("expected error for overwriting existing destination")
	}
}

// ── write_file creates dirs test ──

func TestWriteFileCreatesDirs(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeWriteFileTool(&safeDir)
	_, err := tool.Execute(context.Background(), map[string]any{
		"path":    "a/b/c/deep.txt",
		"content": "nested file",
	})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if _, err := os.Stat(filepath.Join(safeDir, "a", "b", "c", "deep.txt")); err != nil {
		t.Error("nested file should exist after write")
	}
}

// ── project_tree empty dir test ──

func TestProjectTreeEmptyDir(t *testing.T) {
	safeDir := t.TempDir()
	// Create an empty directory (no .git, no files)
	tool := makeProjectTreeTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"path": "."})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "empty directory") {
		t.Errorf("result for empty dir should mention 'empty directory': %q", result)
	}
}

// ── grep tests ──

func TestGrepFilesRecursive(t *testing.T) {
	safeDir := t.TempDir()
	os.MkdirAll(filepath.Join(safeDir, "sub"), 0755)
	os.WriteFile(filepath.Join(safeDir, "sub", "test.txt"), []byte("hello world\n"), 0644)

	tool := makeGrepFilesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"pattern": "hello"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "hello") {
		t.Errorf("result should contain matched line: %q", result)
	}
}

func TestGrepFilesSkipsGit(t *testing.T) {
	safeDir := t.TempDir()
	os.MkdirAll(filepath.Join(safeDir, ".git"), 0755)
	os.WriteFile(filepath.Join(safeDir, ".git", "config"), []byte("secret"), 0644)
	os.WriteFile(filepath.Join(safeDir, "readme.md"), []byte("hello"), 0644)

	tool := makeGrepFilesTool(&safeDir, nil)
	result, err := tool.Execute(context.Background(), map[string]any{"pattern": "secret"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "(no matches)" {
		t.Errorf("should not find 'secret' in .git: %q", result)
	}
}

func TestGrepFilesRegexError(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeGrepFilesTool(&safeDir, nil)
	_, err := tool.Execute(context.Background(), map[string]any{"pattern": "[invalid"})
	if err == nil {
		t.Fatal("expected error for invalid regex")
	}
}

// ── bash tests ──

func TestRunBashExitCode(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"command": "exit 42"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "42") {
		t.Errorf("result should contain exit code: %q", result)
	}
}

func TestRunBashStderr(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"command": "echo stderr-msg >&2"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "stderr-msg") {
		t.Errorf("result should contain stderr output: %q", result)
	}
}

func TestRunBashOutputCappedLines(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	// Generate >500 lines of output
	result, err := tool.Execute(context.Background(), map[string]any{"command": "for i in $(seq 1 600); do echo \"line $i\"; done"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "truncated") {
		t.Errorf("result should mention truncation: %q", result)
	}
}

func TestRunBashOutputCappedChars(t *testing.T) {
	safeDir := t.TempDir()
	tool := makeRunBashTool(&safeDir)
	// Generate >16000 chars of output
	result, err := tool.Execute(context.Background(), map[string]any{"command": "for i in $(seq 1 200); do printf '%.100d' $i; done"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "truncated") {
		t.Errorf("result should mention truncation: %q", result)
	}
}

func TestRunBashWorkingDir(t *testing.T) {
	safeDir := t.TempDir()
	os.WriteFile(filepath.Join(safeDir, "marker.txt"), []byte("here"), 0644)

	tool := makeRunBashTool(&safeDir)
	result, err := tool.Execute(context.Background(), map[string]any{"command": "ls marker.txt"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "marker.txt") {
		t.Errorf("result should contain marker.txt (running in safeDir): %q", result)
	}
}
