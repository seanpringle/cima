package tools

import (
	"path/filepath"
	"strings"
)

// ResolvePath resolves rawPath against safeDir and checks it's within allowed bounds.
// If rawPath is relative, it's resolved relative to safeDir.
// The resolved path must be under safeDir or one of the extraAllowed paths.
// Symlinks in the resolved path are followed to their canonical location.
func ResolvePath(rawPath, safeDir string, extraAllowed ...string) (string, error) {
	if rawPath == "" {
		return "", &ToolError{Message: "path is required"}
	}

	// Normalize safeDir to canonical form
	sd, err := filepath.EvalSymlinks(safeDir)
	if err != nil {
		// If safeDir doesn't exist or can't be resolved, use it as-is
		sd = filepath.Clean(safeDir)
	}

	// Resolve the path
	var resolved string
	if filepath.IsAbs(rawPath) {
		resolved = filepath.Clean(rawPath)
	} else {
		resolved = filepath.Join(sd, rawPath)
	}

	// Try to resolve symlinks in the result
	if eval, err := filepath.EvalSymlinks(resolved); err == nil {
		resolved = eval
	} else {
		resolved = filepath.Clean(resolved)
	}

	// Check that resolved path is under safeDir
	sdWithSep := sd + string(filepath.Separator)
	if resolved == sd || strings.HasPrefix(resolved, sdWithSep) {
		return resolved, nil
	}

	// Check extra allowed paths
	for _, allowed := range extraAllowed {
		allowed = filepath.Clean(allowed)
		allowedWithSep := allowed + string(filepath.Separator)
		if resolved == allowed || strings.HasPrefix(resolved, allowedWithSep) {
			return resolved, nil
		}
	}

	return "", &ToolError{Message: "path must be under " + sd}
}
