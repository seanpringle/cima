package tools

import (
	"context"
	"fmt"
	"strings"
)

// Registry manages a set of tools that the LLM can call.
type Registry struct {
	tools []Tool
}

// NewRegistry creates an empty tool registry.
func NewRegistry() *Registry {
	return &Registry{
		tools: make([]Tool, 0),
	}
}

// Add registers a tool.
func (r *Registry) Add(t Tool) {
	r.tools = append(r.tools, t)
}

// Tools returns a copy of the registered tools list.
func (r *Registry) Tools() []Tool {
	result := make([]Tool, len(r.tools))
	copy(result, r.tools)
	return result
}

// AddDefaults registers all standard tools (filesystem, file I/O, bash, grep, web, git, worktree).
// Plan tools (write_plan, read_plan, comment_plan) are NOT included — they require a PlanBoard
// reference and must be added separately via Add().
func (r *Registry) AddDefaults(safeDir, worktreeBase string, searchParams ...string) {
	var searchAPIKey, searchEngineID, searchEndpoint string
	if len(searchParams) > 0 {
		searchAPIKey = searchParams[0]
	}
	if len(searchParams) > 1 {
		searchEngineID = searchParams[1]
	}
	if len(searchParams) > 2 {
		searchEndpoint = searchParams[2]
	}

	sdPtr := &safeDir
	readOnlyPaths := []string{"/usr/include", "/usr/share/doc"}

	// Read-only tools
	for _, t := range readOnlyTools(sdPtr, readOnlyPaths) {
		r.Add(t)
	}

	// Write tools
	for _, t := range writeTools(sdPtr) {
		r.Add(t)
	}

	// Web tools
	for _, t := range webTools(searchAPIKey, searchEngineID, searchEndpoint) {
		r.Add(t)
	}

	// Git tools are included in readOnlyTools (git_status, git_diff, git_log)
	// and writeTools (git_add, git_commit) above — no separate call needed.

	// Worktree tools
	for _, t := range worktreeTools(sdPtr, &safeDir, worktreeBase) {
		r.Add(t)
	}
}

// ToOpenAITools returns the list of tools in OpenAI function-calling format.
func (r *Registry) ToOpenAITools() []map[string]any {
	return r.ToOpenAIToolsFiltered(nil)
}

// ToOpenAIToolsFiltered returns tools filtered to only include those in the filter set.
func (r *Registry) ToOpenAIToolsFiltered(onlyThese map[string]bool) []map[string]any {
	result := make([]map[string]any, 0, len(r.tools))
	for _, t := range r.tools {
		if onlyThese != nil && !onlyThese[t.Name] {
			continue
		}
		result = append(result, map[string]any{
			"type":     "function",
			"function": t.ToOpenAIFunction(),
		})
	}
	return result
}

// Execute runs a tool by name with the given JSON arguments string.
func (r *Registry) Execute(ctx context.Context, name, argsJSON string) (string, error) {
	// Find the tool
	var tool *Tool
	for i := range r.tools {
		if r.tools[i].Name == name {
			tool = &r.tools[i]
			break
		}
	}
	if tool == nil {
		return "", fmt.Errorf("unknown tool: %s", name)
	}

	// Parse arguments
	args, err := UnmarshalArgs(argsJSON)
	if err != nil {
		return "", err
	}

	return tool.Execute(ctx, args)
}

// ToolNamesByPermission returns the names of all tools with the given permission level.
func (r *Registry) ToolNamesByPermission(perm ToolPermission) []string {
	var names []string
	for _, t := range r.tools {
		if t.Permission == perm {
			names = append(names, t.Name)
		}
	}
	return names
}

// ── Helper: tool factory groups ──

func readOnlyTools(sdPtr *string, readOnlyPaths []string) []Tool {
	return []Tool{
		makeListFilesTool(sdPtr, readOnlyPaths),
		makeReadFileTool(sdPtr, readOnlyPaths),
		makeReadFileLinesTool(sdPtr, readOnlyPaths),
		makeGrepFilesTool(sdPtr, readOnlyPaths),
		makeProjectTreeTool(sdPtr, readOnlyPaths),
		makeGitStatusTool(sdPtr),
		makeGitDiffTool(sdPtr),
		makeGitLogTool(sdPtr),
	}
}

func writeTools(sdPtr *string) []Tool {
	return []Tool{
		makeWriteFileTool(sdPtr),
		makeEditFileTool(sdPtr),
		makeRunBashTool(sdPtr),
		makeGitAddTool(sdPtr),
		makeGitCommitTool(sdPtr),
		makeDeleteFileTool(sdPtr),
		makeMoveFileTool(sdPtr),
		makeRenameFileTool(sdPtr),
	}
}

func webTools(apiKey, engineID, endpoint string) []Tool {
	return []Tool{
		makeWebSearchTool(apiKey, engineID, endpoint),
		makeWebFetchTool(),
	}
}

func worktreeTools(sdPtr *string, sharedSafeDir *string, worktreeBase string) []Tool {
	return []Tool{
		makeStartWorktreeTool(sdPtr, worktreeBase),
		makeStopWorktreeTool(sdPtr, sharedSafeDir),
	}
}

// ── Helper for building stdlib-compatible descriptions ──

func toolDesc(name, desc string) string {
	return strings.TrimSpace(name + " - " + desc)
}
