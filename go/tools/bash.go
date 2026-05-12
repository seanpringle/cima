package tools

import (
	"bytes"
	"context"
	"fmt"
	"os"
	"os/exec"
	"strings"
	"time"
)

func makeRunBashTool(sdPtr *string) Tool {
	return Tool{
		Name:        "run_bash",
		Description: "Run a bash command in the project directory (e.g. build, test, lint). Output is capped at 500 lines / 16000 chars.",
		Permission:  PermissionWrite,
		TimeoutSec:  30,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"command": map[string]any{
					"type":        "string",
					"description": "Shell command to execute",
				},
			},
			"required": []string{"command"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			command, _ := args["command"].(string)
			if command == "" {
				return "", &ToolError{Message: "command is required"}
			}

			// Determine timeout from env var or default
			timeoutSec := 30
			if v := os.Getenv("LLM_BASH_TIMEOUT"); v != "" {
				if t, err := fmt.Sscanf(v, "%d", &timeoutSec); err == nil && t == 1 && timeoutSec > 0 {
					// Use parsed value
				}
			}

			ctx, cancel := context.WithTimeout(ctx, time.Duration(timeoutSec)*time.Second)
			defer cancel()

			cmd := exec.CommandContext(ctx, "sh", "-c", command)
			cmd.Dir = *sdPtr

			var stdout, stderr bytes.Buffer
			cmd.Stdout = &stdout
			cmd.Stderr = &stderr

			err := cmd.Run()

			// Combine stdout and stderr
			output := stdout.String()
			if stderr.Len() > 0 {
				if output != "" {
					output += "\n"
				}
				output += stderr.String()
			}

			// Truncate lines
			lineCount := 0
			for i := 0; i < len(output); i++ {
				if output[i] == '\n' {
					lineCount++
				}
			}
			if lineCount > 500 {
				// Keep first 500 lines
				lines := strings.SplitN(output, "\n", 501)
				output = strings.Join(lines[:500], "\n") + "\n...(truncated, >500 lines)"
			}

			// Truncate chars
			if len(output) > 16000 {
				output = output[:16000] + "...(truncated, >16000 chars)"
			}

			if err != nil {
				if ctx.Err() == context.DeadlineExceeded {
					output += "\n(command timed out after " + fmt.Sprintf("%d", timeoutSec) + "s)"
				} else {
					output += "\n" + err.Error()
				}
			}

			return output, nil
		},
	}
}
