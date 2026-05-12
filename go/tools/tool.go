package tools

import (
	"context"
	"encoding/json"
)

// ToolPermission represents the permission level of a tool.
type ToolPermission int

const (
	PermissionReadOnly  ToolPermission = iota // Safe concurrent read-only tools
	PermissionWrite                           // Mutually exclusive write tools
	PermissionInternal                        // Internal tools (worktree, plan)
)

// ToolError is an error type that tools can return for structured error handling.
type ToolError struct {
	Message string
}

func (e *ToolError) Error() string {
	return e.Message
}

// AsToolError checks if err is a *ToolError (or can be unwrapped to one).
func AsToolError(err error, target **ToolError) bool {
	if err == nil {
		return false
	}
	if te, ok := err.(*ToolError); ok {
		*target = te
		return true
	}
	return false
}

// Tool represents a single agent tool that the LLM can invoke.
type Tool struct {
	// Name is the function name used in the OpenAI function-calling API.
	Name string
	// Description tells the LLM what this tool does.
	Description string
	// Parameters is a JSON Schema describing the tool's arguments.
	Parameters map[string]any
	// Permission controls how the tool can be scheduled (parallel vs serial).
	Permission ToolPermission
	// Timeout is the maximum duration for tool execution (0 = no timeout).
	TimeoutSec int
	// Execute runs the tool with the given parsed arguments.
	Execute func(ctx context.Context, args map[string]any) (string, error)
}

// ToOpenAIFunction returns the OpenAI function definition for this tool.
func (t *Tool) ToOpenAIFunction() map[string]any {
	return map[string]any{
		"name":        t.Name,
		"description": t.Description,
		"parameters":  t.Parameters,
	}
}

// MarshalJSON serializes args as JSON bytes for the tool execution pathway.
func MarshalArgs(args map[string]any) (string, error) {
	b, err := json.Marshal(args)
	if err != nil {
		return "", &ToolError{Message: "invalid JSON arguments: " + err.Error()}
	}
	return string(b), nil
}

// UnmarshalArgs parses a JSON string into a map for tool execution.
func UnmarshalArgs(argsJSON string) (map[string]any, error) {
	var args map[string]any
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return nil, &ToolError{Message: "invalid JSON arguments: " + err.Error()}
	}
	return args, nil
}

// getIntArg extracts an integer argument from a map, handling both float64
// (from JSON unmarshal) and int (from direct Go map literal).
func getIntArg(args map[string]any, key string) int {
	switch v := args[key].(type) {
	case float64:
		return int(v)
	case int:
		return v
	case int64:
		return int(v)
	default:
		return 0
	}
}

// getStringArg extracts a string argument from a map.
func getStringArg(args map[string]any, key string) string {
	s, _ := args[key].(string)
	return s
}

// getBoolArg extracts a boolean argument from a map.
func getBoolArg(args map[string]any, key string) bool {
	b, _ := args[key].(bool)
	return b
}
