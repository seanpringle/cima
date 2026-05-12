package tools

import (
	"context"
	"testing"
)

func TestToolToOpenAIFunction(t *testing.T) {
	tool := Tool{
		Name:        "test_tool",
		Description: "A test tool",
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"input": map[string]any{
					"type": "string",
				},
			},
		},
		Permission: PermissionReadOnly,
	}

	fn := tool.ToOpenAIFunction()

	if fn["name"] != "test_tool" {
		t.Errorf("name = %v, want 'test_tool'", fn["name"])
	}
	if fn["description"] != "A test tool" {
		t.Errorf("description = %v", fn["description"])
	}
	params, ok := fn["parameters"].(map[string]any)
	if !ok {
		t.Fatal("parameters should be a map")
	}
	if params["type"] != "object" {
		t.Errorf("parameters.type = %v", params["type"])
	}
}

func TestToolExecute(t *testing.T) {
	tool := Tool{
		Name:        "echo",
		Description: "Echoes input",
		Parameters:  map[string]any{"type": "object"},
		Permission:  PermissionReadOnly,
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			return "hello", nil
		},
	}

	result, err := tool.Execute(context.Background(), nil)
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "hello" {
		t.Errorf("result = %q, want %q", result, "hello")
	}
}

func TestToolExecuteWithArgs(t *testing.T) {
	tool := Tool{
		Name:        "echo",
		Description: "Echoes input",
		Parameters:  map[string]any{"type": "object"},
		Permission:  PermissionReadOnly,
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			return args["msg"].(string), nil
		},
	}

	result, err := tool.Execute(context.Background(), map[string]any{"msg": "world"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "world" {
		t.Errorf("result = %q, want %q", result, "world")
	}
}

func TestToolPermissions(t *testing.T) {
	tests := []struct {
		perm ToolPermission
		name string
	}{
		{PermissionReadOnly, "ReadOnly"},
		{PermissionWrite, "Write"},
		{PermissionInternal, "Internal"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			tool := Tool{
				Name:       tt.name,
				Permission: tt.perm,
				Execute:    func(ctx context.Context, args map[string]any) (string, error) { return "", nil },
			}
			if tool.Permission != tt.perm {
				t.Errorf("Permission = %v, want %v", tool.Permission, tt.perm)
			}
		})
	}
}

func TestToolExecuteError(t *testing.T) {
	tool := Tool{
		Name:        "failing",
		Description: "Always fails",
		Parameters:  map[string]any{"type": "object"},
		Permission:  PermissionWrite,
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			return "", &ToolError{Message: "something went wrong"}
		},
	}

	_, err := tool.Execute(context.Background(), nil)
	if err == nil {
		t.Fatal("expected error")
	}
	var toolErr *ToolError
	if !AsToolError(err, &toolErr) {
		t.Fatalf("expected ToolError, got %T", err)
	}
	if toolErr.Message != "something went wrong" {
		t.Errorf("Message = %q", toolErr.Message)
	}
}
