package plan

import (
	"fmt"
	"strings"
)

// PlanBoard is a per-session plan document store.
// It holds a single plan markdown body plus an append-only list of comments.
type PlanBoard struct {
	body     string
	comments []string
}

// New creates a new empty PlanBoard.
func New() *PlanBoard {
	return &PlanBoard{}
}

// WritePlan replaces the plan body with new markdown content.
// Previously appended comments are cleared.
func (b *PlanBoard) WritePlan(markdown string) error {
	b.body = markdown
	b.comments = nil
	return nil
}

// ReadPlan returns the current plan body plus any comments formatted as a
// single markdown document. If the plan body is empty, returns "(empty plan)".
func (b *PlanBoard) ReadPlan() (string, error) {
	if b.body == "" {
		return "(empty plan)", nil
	}

	var sb strings.Builder
	sb.WriteString("# Plan\n\n")
	sb.WriteString(b.body)
	sb.WriteString("\n")

	if len(b.comments) > 0 {
		sb.WriteString("\n---\n\n## Comments\n\n")
		for i, comment := range b.comments {
			sb.WriteString(fmt.Sprintf("### Comment %d\n\n", i+1))
			sb.WriteString(comment)
			sb.WriteString("\n\n")
		}
	}

	return sb.String(), nil
}

// CommentPlan appends a comment to the plan document.
// Returns an error if the comment is empty.
func (b *PlanBoard) CommentPlan(comment string) error {
	if comment == "" {
		return fmt.Errorf("comment must not be empty")
	}
	b.comments = append(b.comments, comment)
	return nil
}
