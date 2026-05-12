package config

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestDefaults(t *testing.T) {
	// Ensure no env vars are set that would interfere
	unsetAllEnv(t)
	cfg := FromEnv()

	if cfg.APIBase != "http://127.0.0.1:11000/v1" {
		t.Errorf("default APIBase = %q, want %q", cfg.APIBase, "http://127.0.0.1:11000/v1")
	}
	if cfg.APIKey != "" {
		t.Errorf("default APIKey = %q, want empty", cfg.APIKey)
	}
	if cfg.Model != "deepseek-v4-flash" {
		t.Errorf("default Model = %q, want %q", cfg.Model, "deepseek-v4-flash")
	}
	if cfg.ReasoningEffort != "high" {
		t.Errorf("default ReasoningEffort = %q, want %q", cfg.ReasoningEffort, "high")
	}
	if cfg.MaxToolIterations != 100 {
		t.Errorf("default MaxToolIterations = %d, want 100", cfg.MaxToolIterations)
	}
	if cfg.ContextLimit != 300000 {
		t.Errorf("default ContextLimit = %d, want 300000", cfg.ContextLimit)
	}
	if cfg.CompactThreshold != 90 {
		t.Errorf("default CompactThreshold = %d, want 90", cfg.CompactThreshold)
	}
	if cfg.SafeDir == "" {
		t.Error("default SafeDir should not be empty")
	}
	if len(cfg.ReadOnlyPaths) == 0 {
		t.Error("default ReadOnlyPaths should not be empty")
	}
	if !strings.Contains(cfg.SystemPrompt, "coding assistant") {
		t.Errorf("default SystemPrompt should mention 'coding assistant', got: %q", cfg.SystemPrompt)
	}
}

func TestAPIBaseEnv(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_API", "https://api.example.com/v1")
	cfg := FromEnv()
	if cfg.APIBase != "https://api.example.com/v1" {
		t.Errorf("LLM_API override: got %q, want %q", cfg.APIBase, "https://api.example.com/v1")
	}
}

func TestAPIBaseFallback(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("API_BASE", "https://fallback.example.com/v1")
	cfg := FromEnv()
	if cfg.APIBase != "https://fallback.example.com/v1" {
		t.Errorf("API_BASE fallback: got %q, want %q", cfg.APIBase, "https://fallback.example.com/v1")
	}
}

func TestAPIBasePriority(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_API", "https://primary.example.com/v1")
	t.Setenv("API_BASE", "https://fallback.example.com/v1")
	cfg := FromEnv()
	if cfg.APIBase != "https://primary.example.com/v1" {
		t.Errorf("LLM_API should take priority: got %q", cfg.APIBase)
	}
}

func TestAPIKeyEnv(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_KEY", "sk-test-key")
	cfg := FromEnv()
	if cfg.APIKey != "sk-test-key" {
		t.Errorf("LLM_KEY: got %q, want %q", cfg.APIKey, "sk-test-key")
	}
}

func TestAPIKeyFallback(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("API_KEY", "sk-test-key")
	cfg := FromEnv()
	if cfg.APIKey != "sk-test-key" {
		t.Errorf("API_KEY fallback: got %q, want %q", cfg.APIKey, "sk-test-key")
	}
}

func TestModelEnv(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("MODEL", "gpt-4")
	cfg := FromEnv()
	if cfg.Model != "gpt-4" {
		t.Errorf("MODEL: got %q, want %q", cfg.Model, "gpt-4")
	}
}

func TestSafeDirEnv(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("SAFE_DIR", "/tmp/cima-test")
	cfg := FromEnv()
	// Should be resolved to canonical form
	// /tmp might be symlinked; just check it ends with cima-test
	if !strings.HasSuffix(cfg.SafeDir, "cima-test") {
		t.Errorf("SAFE_DIR: got %q, want suffix cima-test", cfg.SafeDir)
	}
}

func TestSafeDirDefaultsToCWD(t *testing.T) {
	unsetAllEnv(t)
	cfg := FromEnv()
	cwd, _ := os.Getwd()
	if cfg.SafeDir != cwd {
		t.Errorf("default SafeDir = %q, want cwd %q", cfg.SafeDir, cwd)
	}
}

func TestMaxToolIterations(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_MAX_TOOL_ITERATIONS", "50")
	cfg := FromEnv()
	if cfg.MaxToolIterations != 50 {
		t.Errorf("got %d, want 50", cfg.MaxToolIterations)
	}
}

func TestMaxToolIterationsInvalid(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_MAX_TOOL_ITERATIONS", "-5")
	cfg := FromEnv()
	if cfg.MaxToolIterations != 100 {
		t.Errorf("invalid value should fall back to default: got %d, want 100", cfg.MaxToolIterations)
	}
}

func TestContextLimit(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_CONTEXT_LIMIT", "128000")
	cfg := FromEnv()
	if cfg.ContextLimit != 128000 {
		t.Errorf("got %d, want 128000", cfg.ContextLimit)
	}
}

func TestCompactThreshold(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_COMPACT_THRESHOLD", "75")
	cfg := FromEnv()
	if cfg.CompactThreshold != 75 {
		t.Errorf("got %d, want 75", cfg.CompactThreshold)
	}
}

func TestCompactThresholdClamped(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_COMPACT_THRESHOLD", "150")
	cfg := FromEnv()
	if cfg.CompactThreshold != 100 {
		t.Errorf(">100 should clamp to 100: got %d, want 100", cfg.CompactThreshold)
	}
}

func TestReadOnlyPaths(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("READ_ONLY_PATHS", "/opt/include:/usr/local/include")
	cfg := FromEnv()
	found := false
	for _, p := range cfg.ReadOnlyPaths {
		if strings.HasSuffix(p, "opt/include") || p == "/opt/include" {
			found = true
		}
	}
	if !found {
		t.Errorf("ReadOnlyPaths should contain /opt/include, got %v", cfg.ReadOnlyPaths)
	}
}

func TestSearchParams(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("SEARCH_API_KEY", "test-key")
	t.Setenv("SEARCH_ENGINE_ID", "test-engine")
	t.Setenv("SEARCH_ENDPOINT", "https://custom.example.com/search?q={query}")
	cfg := FromEnv()
	if cfg.SearchAPIKey != "test-key" {
		t.Errorf("SearchAPIKey: got %q", cfg.SearchAPIKey)
	}
	if cfg.SearchEngineID != "test-engine" {
		t.Errorf("SearchEngineID: got %q", cfg.SearchEngineID)
	}
	if cfg.SearchEndpoint != "https://custom.example.com/search?q={query}" {
		t.Errorf("SearchEndpoint: got %q", cfg.SearchEndpoint)
	}
}

func TestWorktreeBase(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("WORKTREE_BASE", "/tmp/cima-worktrees")
	cfg := FromEnv()
	if cfg.WorktreeBase != "/tmp/cima-worktrees" {
		t.Errorf("WorktreeBase: got %q", cfg.WorktreeBase)
	}
}

func TestSystemPromptEnv(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_SYSTEM_PROMPT", "You are a helpful assistant.")
	cfg := FromEnv()
	if cfg.SystemPrompt != "You are a helpful assistant." {
		t.Errorf("SystemPrompt: got %q", cfg.SystemPrompt)
	}
}

func TestSystemPromptFallback(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("SYSTEM_PROMPT", "Be concise.")
	cfg := FromEnv()
	if cfg.SystemPrompt != "Be concise." {
		t.Errorf("SYSTEM_PROMPT fallback: got %q", cfg.SystemPrompt)
	}
}

func TestReasoningEffort(t *testing.T) {
	unsetAllEnv(t)
	t.Setenv("LLM_REASONING_EFFORT", "low")
	cfg := FromEnv()
	if cfg.ReasoningEffort != "low" {
		t.Errorf("ReasoningEffort: got %q", cfg.ReasoningEffort)
	}
}

func TestSafeDirCanonical(t *testing.T) {
	unsetAllEnv(t)
	tmp := t.TempDir()
	err := os.MkdirAll(filepath.Join(tmp, "subdir"), 0755)
	if err != nil {
		t.Fatal(err)
	}
	t.Setenv("SAFE_DIR", filepath.Join(tmp, "subdir"))
	cfg := FromEnv()
	// Should be canonical
	if !strings.HasSuffix(cfg.SafeDir, "subdir") {
		t.Errorf("SafeDir = %q, should end with subdir", cfg.SafeDir)
	}
}

func TestDefaultSystemPrompt(t *testing.T) {
	unsetAllEnv(t)
	cfg := FromEnv()
	if !strings.Contains(cfg.SystemPrompt, "markdown") {
		t.Errorf("default SystemPrompt should mention markdown, got: %q", cfg.SystemPrompt)
	}
	if !strings.Contains(cfg.SystemPrompt, "Plan document") {
		t.Errorf("default SystemPrompt should mention Plan document, got: %q", cfg.SystemPrompt)
	}
}

// unsetAllEnv clears all env vars that could affect config loading.
func unsetAllEnv(t *testing.T) {
	t.Helper()
	keys := []string{
		"LLM_API", "API_BASE",
		"LLM_KEY", "API_KEY",
		"MODEL",
		"LLM_REASONING_EFFORT", "REASONING_EFFORT",
		"LLM_SYSTEM_PROMPT", "SYSTEM_PROMPT",
		"SAFE_DIR",
		"LLM_MAX_TOOL_ITERATIONS",
		"LLM_CONTEXT_LIMIT",
		"LLM_COMPACT_THRESHOLD",
		"SEARCH_API_KEY", "SEARCH_ENGINE_ID", "SEARCH_ENDPOINT",
		"WORKTREE_BASE",
		"READ_ONLY_PATHS",
	}
	for _, k := range keys {
		os.Unsetenv(k)
	}
}
