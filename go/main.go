package main

import (
	"flag"
	"fmt"
	"os"

	"cima/app"
	"cima/config"
	"cima/tools"

	"fyne.io/fyne/v2"
	fyneApp "fyne.io/fyne/v2/app"
)

// Version is set at build time via -ldflags. Defaults to "dev".
var Version = "dev"

const usageText = `cima — AI coding assistant (Go/Fyne port)

Usage:
  cima [flags]

Flags:
  -h, --help    Show this help message and exit

Environment variables:
  LLM_API       OpenAI-compatible API endpoint (default: http://127.0.0.1:11000/v1)
  LLM_KEY       API authentication key
  MODEL         Model name (default: deepseek-v4-flash)
  SAFE_DIR      Sandbox directory for tool operations (default: current directory)
  For the full list, see the config package documentation.
`

func main() {
	// Parse flags
	help := flag.Bool("help", false, "Show help message")
	flag.BoolVar(help, "h", false, "Show help message")
	flag.Parse()

	if *help || flag.NArg() > 0 {
		fmt.Print(usageText)
		os.Exit(0)
	}

	// Print version
	fmt.Fprintf(os.Stderr, "cima %s starting...\n", Version)

	// Load configuration
	cfg := config.FromEnv()

	// Validate essential config
	if cfg.APIBase == "" {
		fmt.Fprintln(os.Stderr, "fatal: LLM_API or API_BASE must be set")
		os.Exit(1)
	}

	// Validate SAFE_DIR if set explicitly (non-default)
	defaultSafeDir, _ := os.Getwd()
	if cfg.SafeDir != defaultSafeDir {
		if info, err := os.Stat(cfg.SafeDir); err != nil {
			fmt.Fprintf(os.Stderr, "warning: SAFE_DIR %s does not exist: %v\n", cfg.SafeDir, err)
		} else if !info.IsDir() {
			fmt.Fprintf(os.Stderr, "warning: SAFE_DIR %s is not a directory\n", cfg.SafeDir)
		}
	}

	// Check worktree base exists
	worktreeBase := cfg.WorktreeBase
	if worktreeBase == "" {
		worktreeBase = "/tmp/cima"
	}
	if _, err := os.Stat(worktreeBase); os.IsNotExist(err) {
		if err := os.MkdirAll(worktreeBase, 0755); err != nil {
			fmt.Fprintf(os.Stderr, "warning: could not create worktree base %s: %v\n", worktreeBase, err)
		}
	}

	// Use dark theme via env var (read during settings init, avoids
	// the "fyne.Do[AndWait] called from main goroutine" panic from SetTheme)
	os.Setenv("FYNE_THEME", "dark")

	// Create the Fyne application
	fa := fyneApp.NewWithID("cima")

	// Create the main window
	window := fa.NewWindow("cima")
	window.Resize(fyne.NewSize(1280, 720))

	// Create the application, passing the window for shortcuts/menus
	appInstance := app.NewApp(cfg, window)
	window.SetContent(appInstance.Content())

	// Set up cleanup on window close
	window.SetCloseIntercept(func() {
		// Cancel all running chats and clean up
		appInstance.Shutdown()
		// Clean up any active worktree
		tools.CleanupWorktree()
		// Close the window
		window.Close()
	})


	window.ShowAndRun()
}


