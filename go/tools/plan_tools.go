package tools

import (
	"context"
	"strings"

	"cima/plan"
)

func MakeWritePlanTool(pb *plan.PlanBoard) Tool {
	return Tool{
		Name:        "write_plan",
		Description: "Write the Plan document. This completely replaces the plan body (previous comments are cleared). Use this from the Planner to document the implementation plan for the Builder.",
		Permission:  PermissionWrite,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"markdown": map[string]any{
					"type":        "string",
					"description": "Markdown content of the plan",
				},
			},
			"required": []string{"markdown"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			markdown, _ := args["markdown"].(string)
			if err := pb.WritePlan(markdown); err != nil {
				return "", err
			}
			return "Plan written. Use read_plan() to view it.", nil
		},
	}
}

func MakeReadPlanTool(pb *plan.PlanBoard) Tool {
	return Tool{
		Name:        "read_plan",
		Description: "Read the Plan document (plan body + comments). Returns a markdown document with the plan and any comments.",
		Permission:  PermissionReadOnly,
		Parameters: map[string]any{
			"type":       "object",
			"properties": map[string]any{},
			"required":   []string{},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			content, err := pb.ReadPlan()
			if err != nil {
				return "", err
			}
			return content, nil
		},
	}
}

func MakeCommentPlanTool(pb *plan.PlanBoard) Tool {
	return Tool{
		Name:        "comment_plan",
		Description: "Append a comment to the Plan document. Comments are preserved separately from the plan body and listed after it. Use this for progress updates, review feedback, or change requests.",
		Permission:  PermissionReadOnly,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"comment": map[string]any{
					"type":        "string",
					"description": "Markdown comment to append to the plan",
				},
			},
			"required": []string{"comment"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			comment, _ := args["comment"].(string)
			if strings.TrimSpace(comment) == "" {
				return "", &ToolError{Message: "comment must not be empty"}
			}
			if err := pb.CommentPlan(comment); err != nil {
				return "", err
			}
			return "Comment added to plan.", nil
		},
	}
}
