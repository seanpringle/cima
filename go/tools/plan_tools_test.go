package tools

import (
	"context"
	"strings"
	"testing"

	"cima/plan"
)

func TestWritePlanTool(t *testing.T) {
	pb := plan.New()
	tool := MakeWritePlanTool(pb)
	_, err := tool.Execute(context.Background(), map[string]any{"markdown": "Test plan body"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}

	content, _ := pb.ReadPlan()
	if !strings.Contains(content, "Test plan body") {
		t.Errorf("plan should contain body: %q", content)
	}
}

func TestReadPlanTool(t *testing.T) {
	pb := plan.New()
	tool := MakeReadPlanTool(pb)
	result, err := tool.Execute(context.Background(), map[string]any{})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if result != "(empty plan)" {
		t.Errorf("empty plan should return '(empty plan)', got %q", result)
	}
}

func TestCommentPlanTool(t *testing.T) {
	pb := plan.New()
	pb.WritePlan("body")

	tool := MakeCommentPlanTool(pb)
	_, err := tool.Execute(context.Background(), map[string]any{"comment": "Nice plan"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}

	content, _ := pb.ReadPlan()
	if !strings.Contains(content, "Nice plan") {
		t.Errorf("plan should contain comment: %q", content)
	}
}

func TestCommentPlanToolEmpty(t *testing.T) {
	pb := plan.New()
	pb.WritePlan("body")

	tool := MakeCommentPlanTool(pb)
	_, err := tool.Execute(context.Background(), map[string]any{"comment": ""})
	if err == nil {
		t.Fatal("expected error for empty comment")
	}
}

func TestPlanToolsInRegistry(t *testing.T) {
	pb := plan.New()
	r := NewRegistry()
	safeDir := t.TempDir()
	r.AddDefaults(safeDir, "")
	r.Add(MakeWritePlanTool(pb))
	r.Add(MakeReadPlanTool(pb))
	r.Add(MakeCommentPlanTool(pb))

	names := make(map[string]bool)
	for _, tool := range r.Tools() {
		names[tool.Name] = true
	}

	for _, name := range []string{"write_plan", "read_plan", "comment_plan"} {
		if !names[name] {
			t.Errorf("plan tool %q not found in registry", name)
		}
	}
}
