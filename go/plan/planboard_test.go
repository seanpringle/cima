package plan

import (
	"strings"
	"testing"
)

func TestEmptyPlan(t *testing.T) {
	b := New()
	content, err := b.ReadPlan()
	if err != nil {
		t.Fatalf("ReadPlan on empty board: %v", err)
	}
	if content != "(empty plan)" {
		t.Errorf("empty plan: got %q, want %q", content, "(empty plan)")
	}
}

func TestWriteAndRead(t *testing.T) {
	b := New()
	err := b.WritePlan("This is the plan body.")
	if err != nil {
		t.Fatalf("WritePlan: %v", err)
	}
	content, err := b.ReadPlan()
	if err != nil {
		t.Fatalf("ReadPlan: %v", err)
	}
	if !strings.Contains(content, "This is the plan body.") {
		t.Errorf("ReadPlan should contain the plan body, got: %q", content)
	}
	if !strings.Contains(content, "# Plan") {
		t.Errorf("ReadPlan should start with # Plan heading")
	}
}

func TestWriteOverwritesBody(t *testing.T) {
	b := New()
	_ = b.WritePlan("First plan")
	_ = b.WritePlan("Second plan")
	content, _ := b.ReadPlan()
	if strings.Contains(content, "First plan") {
		t.Error("ReadPlan should not contain old plan body after overwrite")
	}
	if !strings.Contains(content, "Second plan") {
		t.Error("ReadPlan should contain new plan body after overwrite")
	}
}

func TestCommentAppends(t *testing.T) {
	b := New()
	_ = b.WritePlan("Plan body")
	_ = b.CommentPlan("Great plan!")
	content, _ := b.ReadPlan()
	if !strings.Contains(content, "Great plan!") {
		t.Errorf("ReadPlan should contain the comment, got: %q", content)
	}
}

func TestCommentPreservedAfterWrite(t *testing.T) {
	b := New()
	_ = b.WritePlan("Plan body")
	_ = b.CommentPlan("A comment")
	_ = b.WritePlan("New body")
	content, _ := b.ReadPlan()
	if strings.Contains(content, "A comment") {
		t.Error("Comments should be cleared when plan is overwritten")
	}
	if !strings.Contains(content, "New body") {
		t.Error("New plan body should be present")
	}
}

func TestEmptyCommentError(t *testing.T) {
	b := New()
	_ = b.WritePlan("Plan body")
	err := b.CommentPlan("")
	if err == nil {
		t.Fatal("expected error for empty comment, got nil")
	}
}

func TestMultipleComments(t *testing.T) {
	b := New()
	_ = b.WritePlan("Plan body")
	_ = b.CommentPlan("Comment 1")
	_ = b.CommentPlan("Comment 2")
	_ = b.CommentPlan("Comment 3")
	content, _ := b.ReadPlan()

	if !strings.Contains(content, "Comment 1") {
		t.Error("missing Comment 1")
	}
	if !strings.Contains(content, "Comment 2") {
		t.Error("missing Comment 2")
	}
	if !strings.Contains(content, "Comment 3") {
		t.Error("missing Comment 3")
	}
}

func TestNewlineHandling(t *testing.T) {
	b := New()
	multiLine := "Line 1\nLine 2\n\nLine 4"
	err := b.WritePlan(multiLine)
	if err != nil {
		t.Fatalf("WritePlan: %v", err)
	}
	content, _ := b.ReadPlan()
	if !strings.Contains(content, "Line 1") {
		t.Error("missing Line 1")
	}
	if !strings.Contains(content, "Line 2") {
		t.Error("missing Line 2")
	}
	if !strings.Contains(content, "Line 4") {
		t.Error("missing Line 4")
	}
}

func TestReadPlanFormat(t *testing.T) {
	b := New()
	_ = b.WritePlan("Body text")
	_ = b.CommentPlan("A comment")
	content, _ := b.ReadPlan()

	// Should contain the plan heading
	if !strings.HasPrefix(content, "# Plan") {
		t.Errorf("content should start with # Plan, got: %q[:20]", content)
	}
	// Should contain the plan body
	if !strings.Contains(content, "Body text") {
		t.Errorf("content should contain plan body")
	}
	// Should contain the comments section
	if !strings.Contains(content, "## Comments") {
		t.Errorf("content should have ## Comments section")
	}
	// Should contain individual comment headings
	if !strings.Contains(content, "### Comment 1") {
		t.Errorf("content should have ### Comment 1")
	}
}

func TestReadPlanFormatNoComments(t *testing.T) {
	b := New()
	_ = b.WritePlan("Body only")
	content, _ := b.ReadPlan()

	if strings.Contains(content, "## Comments") {
		t.Error("content should not have ## Comments section when there are no comments")
	}
	if !strings.Contains(content, "Body only") {
		t.Error("content should contain plan body")
	}
}

func TestNewAfterWriteClearsComments(t *testing.T) {
	b := New()
	_ = b.WritePlan("First")
	_ = b.CommentPlan("Old comment")
	_ = b.WritePlan("Second")
	content, _ := b.ReadPlan()

	if strings.Contains(content, "Old comment") {
		t.Error("write_plan should clear previous comments")
	}
}
