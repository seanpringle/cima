package config

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Config holds all configuration for the application, loaded from environment variables.
type Config struct {
	// APIBase is the OpenAI-compatible API endpoint (default: http://127.0.0.1:11000/v1)
	APIBase string

	// APIKey is the authentication key for the API (optional)
	APIKey string

	// Model is the model name to use (default: deepseek-v4-flash)
	Model string

	// ReasoningEffort controls the model's reasoning depth (default: "high")
	ReasoningEffort string

	// SafeDir is the sandbox directory for tool operations (default: current working directory)
	SafeDir string

	// SearchAPIKey is the Google Custom Search API key (optional)
	SearchAPIKey string

	// SearchEngineID is the Google Custom Search engine ID (optional)
	SearchEngineID string

	// SearchEndpoint is a custom search endpoint URL with {query} placeholder (optional)
	SearchEndpoint string

	// WorktreeBase is the parent directory for git worktrees (default: /tmp/cima)
	WorktreeBase string

	// ReadOnlyPaths is a list of additional paths readable by read-only tools
	ReadOnlyPaths []string

	// MaxToolIterations is the maximum number of tool call iterations per user turn (default: 100)
	MaxToolIterations int

	// ContextLimit is the model's context window in tokens (default: 300000)
	ContextLimit int

	// CompactThreshold is the percentage of context limit that triggers compaction (default: 90)
	CompactThreshold int

	// SystemPrompt is the system prompt for chat sessions
	SystemPrompt string
}

// defaultSystemPrompt is the built-in system prompt used when no env var overrides it.
const defaultSystemPrompt = `You are an AI coding assistant.

Use markdown with a neat, clear layout for all output. Be concise.
All of commonmark and github tables supported, but generally prefer lists over tables.

You have access to a markdown Plan document visible to the user. Always start a task by researching the user's instructions and writing your Plan document. Always explicitly ask the user to review and approve your completed Plan document before you start implementation.

When merging a worktree branch back to the repo, always rebase the worktree branch first, rebuild and re-test, then use a clean git push to do a local fast-forward merge from inside your cwd. Do not merge using the main repo checked-out worktree in case merge conflict artifacts interfere with other agents.`

// FromEnv loads configuration from environment variables, applying defaults.
func FromEnv() Config {
	cfg := Config{
		APIBase:           "http://127.0.0.1:11000/v1",
		APIKey:            "",
		Model:             "deepseek-v4-flash",
		ReasoningEffort:   "high",
		WorktreeBase:      "/tmp/cima",
		MaxToolIterations: 100,
		ContextLimit:      300000,
		CompactThreshold:  90,
		SystemPrompt:      defaultSystemPrompt,
		ReadOnlyPaths:     []string{"/usr/include", "/usr/share/doc"},
	}

	// API base: LLM_API takes priority, API_BASE is fallback
	if v := os.Getenv("LLM_API"); v != "" {
		cfg.APIBase = v
	} else if v := os.Getenv("API_BASE"); v != "" {
		cfg.APIBase = v
	}

	// API key: LLM_KEY takes priority, API_KEY is fallback
	if v := os.Getenv("LLM_KEY"); v != "" {
		cfg.APIKey = v
	} else if v := os.Getenv("API_KEY"); v != "" {
		cfg.APIKey = v
	}

	// Model
	if v := os.Getenv("MODEL"); v != "" {
		cfg.Model = v
	}

	// Reasoning effort
	if v := os.Getenv("LLM_REASONING_EFFORT"); v != "" {
		cfg.ReasoningEffort = v
	} else if v := os.Getenv("REASONING_EFFORT"); v != "" {
		cfg.ReasoningEffort = v
	}

	// System prompt
	if v := os.Getenv("LLM_SYSTEM_PROMPT"); v != "" {
		cfg.SystemPrompt = v
	} else if v := os.Getenv("SYSTEM_PROMPT"); v != "" {
		cfg.SystemPrompt = v
	}

	// Safe directory
	if v := os.Getenv("SAFE_DIR"); v != "" {
		cfg.SafeDir = v
	} else {
		cwd, err := os.Getwd()
		if err == nil {
			cfg.SafeDir = cwd
		}
	}

	// Canonicalize safe_dir
	if cfg.SafeDir != "" {
		canonical, err := filepath.EvalSymlinks(cfg.SafeDir)
		if err == nil {
			cfg.SafeDir = canonical
		}
	}

	// Max tool iterations
	if v := os.Getenv("LLM_MAX_TOOL_ITERATIONS"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			cfg.MaxToolIterations = n
		}
	}

	// Context limit
	if v := os.Getenv("LLM_CONTEXT_LIMIT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			cfg.ContextLimit = n
		}
	}

	// Compact threshold
	if v := os.Getenv("LLM_COMPACT_THRESHOLD"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			if n > 100 {
				n = 100
			}
			cfg.CompactThreshold = n
		}
	}

	// Search configuration
	cfg.SearchAPIKey = os.Getenv("SEARCH_API_KEY")
	cfg.SearchEngineID = os.Getenv("SEARCH_ENGINE_ID")
	cfg.SearchEndpoint = os.Getenv("SEARCH_ENDPOINT")

	// Worktree base
	if v := os.Getenv("WORKTREE_BASE"); v != "" {
		cfg.WorktreeBase = v
	}

	// Read-only paths (colon-separated, appended to defaults)
	if v := os.Getenv("READ_ONLY_PATHS"); v != "" {
		parts := strings.Split(v, ":")
		for _, p := range parts {
			p = strings.TrimSpace(p)
			if p == "" {
				continue
			}
			canonical, err := filepath.EvalSymlinks(p)
			if err == nil {
				p = canonical
			}
			cfg.ReadOnlyPaths = append(cfg.ReadOnlyPaths, p)
		}
	}

	return cfg
}
