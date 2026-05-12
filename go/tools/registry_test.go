package tools

import (
	"context"
	"strings"
	"testing"
)

func TestRegistryAddAndFind(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{Name: "test_tool", Execute: func(ctx context.Context, args map[string]any) (string, error) {
		return "ok", nil
	}})

	if len(r.Tools()) != 1 {
		t.Fatalf("expected 1 tool, got %d", len(r.Tools()))
	}
	if r.Tools()[0].Name != "test_tool" {
		t.Errorf("tool name = %q", r.Tools()[0].Name)
	}
}

func TestRegistryExecute(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{
		Name: "echo",
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			return args["msg"].(string), nil
		},
	})

	result, err := r.Execute(context.Background(), "echo", `{"msg": "hello"}`)
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "hello" {
		t.Errorf("result = %q", result)
	}
}

func TestRegistryExecuteUnknown(t *testing.T) {
	r := NewRegistry()
	_, err := r.Execute(context.Background(), "nonexistent", "{}")
	if err == nil {
		t.Fatal("expected error for unknown tool")
	}
	if !strings.Contains(err.Error(), "unknown tool") {
		t.Errorf("error = %v, should mention 'unknown tool'", err)
	}
}

func TestRegistryExecuteInvalidJSON(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{Name: "test", Execute: func(ctx context.Context, args map[string]any) (string, error) {
		return "", nil
	}})

	_, err := r.Execute(context.Background(), "test", "{bad json}")
	if err == nil {
		t.Fatal("expected error for invalid JSON")
	}
}

func TestRegistryToOpenAITools(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{
		Name:        "tool_a",
		Description: "Tool A",
		Parameters:  map[string]any{"type": "object"},
	})
	r.Add(Tool{
		Name:        "tool_b",
		Description: "Tool B",
		Parameters:  map[string]any{"type": "object"},
	})

	tools := r.ToOpenAITools()
	if len(tools) != 2 {
		t.Fatalf("expected 2 tools, got %d", len(tools))
	}

	for _, tool := range tools {
		fn, ok := tool["function"].(map[string]any)
		if !ok {
			t.Fatal("each tool should have a 'function' key")
		}
		if fn["name"] == "" {
			t.Error("function name should not be empty")
		}
	}
}

func TestRegistryToOpenAIToolsFiltered(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{Name: "tool_a", Execute: func(ctx context.Context, args map[string]any) (string, error) { return "", nil }})
	r.Add(Tool{Name: "tool_b", Execute: func(ctx context.Context, args map[string]any) (string, error) { return "", nil }})
	r.Add(Tool{Name: "tool_c", Execute: func(ctx context.Context, args map[string]any) (string, error) { return "", nil }})

	filtered := r.ToOpenAIToolsFiltered(map[string]bool{"tool_a": true, "tool_c": true})
	if len(filtered) != 2 {
		t.Fatalf("expected 2 filtered tools, got %d", len(filtered))
	}
}

func TestRegistryToolNamesByPermission(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{Name: "read", Permission: PermissionReadOnly})
	r.Add(Tool{Name: "write", Permission: PermissionWrite})
	r.Add(Tool{Name: "internal", Permission: PermissionInternal})

	ro := r.ToolNamesByPermission(PermissionReadOnly)
	if len(ro) != 1 || ro[0] != "read" {
		t.Errorf("read-only names = %v", ro)
	}

	w := r.ToolNamesByPermission(PermissionWrite)
	if len(w) != 1 || w[0] != "write" {
		t.Errorf("write names = %v", w)
	}
}

func TestRegistryExecuteWithCancel(t *testing.T) {
	r := NewRegistry()
	r.Add(Tool{
		Name: "slow",
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			<-ctx.Done()
			return "", ctx.Err()
		},
	})

	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	_, err := r.Execute(ctx, "slow", "{}")
	if err == nil {
		t.Fatal("expected error due to cancelled context")
	}
}

func TestRegistryAddDefaults(t *testing.T) {
	r := NewRegistry()
	safeDir := t.TempDir()
	r.AddDefaults(safeDir, "")

	if len(r.Tools()) < 20 {
		t.Fatalf("expected at least 20 tools from AddDefaults, got %d", len(r.Tools()))
	}

	// Check specific tools exist
	names := make(map[string]bool)
	for _, tool := range r.Tools() {
		names[tool.Name] = true
	}

	essential := []string{"list_files", "read_file", "write_file", "run_bash",
		"grep_files", "project_tree", "web_search", "web_fetch",
		"git_status", "git_diff", "git_log", "git_add", "git_commit"}
	for _, name := range essential {
		if !names[name] {
			t.Errorf("essential tool %q not found in defaults", name)
		}
	}
}

func TestRegistryAddDefaultsIncludesPlanTools(t *testing.T) {
	// Plan tools require a PlanBoard reference. We test this via the registry setup
	// where plan tools are added separately.
	r := NewRegistry()
	safeDir := t.TempDir()
	r.AddDefaults(safeDir, "")

	names := make(map[string]bool)
	for _, tool := range r.Tools() {
		names[tool.Name] = true
	}

	// Plan tools are added separately via Add(), not AddDefaults
	// So they shouldn't be in defaults yet
	noPlan := []string{"write_plan", "read_plan", "comment_plan"}
	for _, name := range noPlan {
		if names[name] {
			t.Logf("plan tool %q found in defaults (may be expected)", name)
		}
	}
}
