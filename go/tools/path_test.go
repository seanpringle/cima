package tools

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResolveRelative(t *testing.T) {
	safeDir := t.TempDir()
	resolved, err := ResolvePath("foo", safeDir)
	if err != nil {
		t.Fatalf("ResolvePath: %v", err)
	}
	expected := filepath.Join(safeDir, "foo")
	if resolved != expected {
		t.Errorf("resolved = %q, want %q", resolved, expected)
	}
}

func TestResolveAbsoluteInside(t *testing.T) {
	safeDir := t.TempDir()
	sub := filepath.Join(safeDir, "subdir")
	os.MkdirAll(sub, 0755)

	resolved, err := ResolvePath(sub, safeDir)
	if err != nil {
		t.Fatalf("ResolvePath: %v", err)
	}
	if resolved != sub {
		t.Errorf("resolved = %q, want %q", resolved, sub)
	}
}

func TestResolveOutsideSafeDir(t *testing.T) {
	safeDir := t.TempDir()
	_, err := ResolvePath("/etc/passwd", safeDir)
	if err == nil {
		t.Fatal("expected error for path outside safe dir")
	}
}

func TestResolveEscape(t *testing.T) {
	safeDir := t.TempDir()
	_, err := ResolvePath("../../etc/passwd", safeDir)
	if err == nil {
		t.Fatal("expected error for escape path")
	}
}

func TestResolveCurrentDir(t *testing.T) {
	safeDir := t.TempDir()
	resolved, err := ResolvePath(".", safeDir)
	if err != nil {
		t.Fatalf("ResolvePath: %v", err)
	}
	if resolved != safeDir {
		t.Errorf("resolved = %q, want %q", resolved, safeDir)
	}
}

func TestResolveExtraAllowed(t *testing.T) {
	safeDir := t.TempDir()
	extra := "/usr/include"

	resolved, err := ResolvePath(extra, safeDir, extra)
	if err != nil {
		t.Fatalf("ResolvePath with extra allowed: %v", err)
	}
	if resolved != extra {
		t.Errorf("resolved = %q, want %q", resolved, extra)
	}
}

func TestResolveEmptyPath(t *testing.T) {
	_, err := ResolvePath("", "/tmp")
	if err == nil {
		t.Fatal("expected error for empty path")
	}
}

func TestResolveSiblingDirectoryAllowed(t *testing.T) {
	safeDir := t.TempDir()
	sibling := filepath.Join(safeDir, "..", filepath.Base(safeDir))
	resolved, err := ResolvePath(sibling, safeDir)
	if err != nil {
		t.Fatalf("ResolvePath sibling: %v", err)
	}
	if resolved != safeDir {
		t.Errorf("resolved = %q, want %q", resolved, safeDir)
	}
}

func TestResolveSubdir(t *testing.T) {
	safeDir := t.TempDir()
	sub := filepath.Join(safeDir, "a", "b", "c")
	os.MkdirAll(sub, 0755)

	resolved, err := ResolvePath("a/b/c", safeDir)
	if err != nil {
		t.Fatalf("ResolvePath subdir: %v", err)
	}
	if resolved != sub {
		t.Errorf("resolved = %q, want %q", resolved, sub)
	}
}

func TestResolveAbsoluteSubdirInside(t *testing.T) {
	safeDir := t.TempDir()
	sub := filepath.Join(safeDir, "inside")
	os.MkdirAll(sub, 0755)

	resolved, err := ResolvePath(sub, safeDir)
	if err != nil {
		t.Fatalf("ResolvePath absolute inside: %v", err)
	}
	if resolved != sub {
		t.Errorf("resolved = %q, want %q", resolved, sub)
	}
}

func TestResolveNonexistentPath(t *testing.T) {
	safeDir := t.TempDir()
	// A non-existent path under safeDir should still be allowed (it will be created later)
	resolved, err := ResolvePath("nonexistent", safeDir)
	if err != nil {
		t.Fatalf("ResolvePath nonexistent: %v", err)
	}
	expected := filepath.Join(safeDir, "nonexistent")
	if resolved != expected {
		t.Errorf("resolved = %q, want %q", resolved, expected)
	}
}

func TestResolveExtraAllowedMultiple(t *testing.T) {
	safeDir := t.TempDir()
	extra1 := "/usr/include"
	extra2 := "/usr/share/doc"

	// Should work for first extra
	r, err := ResolvePath(extra1, safeDir, extra1, extra2)
	if err != nil {
		t.Fatalf("extra1: %v", err)
	}
	if r != extra1 {
		t.Errorf("got %q", r)
	}

	// Should work for second extra
	r, err = ResolvePath(extra2, safeDir, extra1, extra2)
	if err != nil {
		t.Fatalf("extra2: %v", err)
	}
	if r != extra2 {
		t.Errorf("got %q", r)
	}
}

func TestResolvePathWithSymlink(t *testing.T) {
	safeDir := t.TempDir()
	realDir := filepath.Join(safeDir, "real")
	os.MkdirAll(realDir, 0755)
	linkDir := filepath.Join(safeDir, "link")
	os.Symlink("real", linkDir)

	resolved, err := ResolvePath("link", safeDir)
	if err != nil {
		t.Fatalf("ResolvePath symlink: %v", err)
	}
	if resolved != realDir {
		t.Errorf("resolved = %q, want %q (resolved symlink)", resolved, realDir)
	}
}

func TestResolveTraversalBlocked(t *testing.T) {
	safeDir := t.TempDir()
	_, err := ResolvePath("/usr/include/../lib", safeDir)
	if err == nil {
		t.Fatal("expected error for traversal outside safe dir")
	}
}

func TestResolveTraversalBlockedRelative(t *testing.T) {
	safeDir := t.TempDir()
	_, err := ResolvePath("foo/../../../etc", safeDir)
	if err == nil {
		t.Fatal("expected error for traversal outside safe dir")
	}
}

func TestResolveNonCanonicalSafeDir(t *testing.T) {
	// safeDir with extra slashes or ".." components
	realDir := t.TempDir()
	safeDir := filepath.Join(realDir, "sub")
	os.MkdirAll(safeDir, 0755)
	// Use a non-canonical safeDir
	weirdSafe := filepath.Join(realDir, "..", filepath.Base(realDir), "sub")

	resolved, err := ResolvePath("file.txt", weirdSafe)
	if err != nil {
		t.Fatalf("ResolvePath with non-canonical safeDir: %v", err)
	}
	expected := filepath.Join(safeDir, "file.txt")
	// The path must be under the canonical safeDir
	if !strings.HasPrefix(resolved, safeDir+string(filepath.Separator)) && resolved != safeDir+"/file.txt" && resolved != expected {
		t.Errorf("resolved = %q, expected prefix %q", resolved, safeDir)
	}
}
