package chat

import (
	"context"
	"errors"
	"fmt"
	"log"
	"strings"
	"sync"

	"cima/client"
	"cima/config"
	"cima/plan"
	"cima/tools"
)

// ChatResult is the final result of a single user turn.
type ChatResult struct {
	Content   string
	Reasoning string
}

// OutputCallback is called during streaming for UI updates.
// text is the streaming text fragment, entryType indicates the kind of content.
type OutputCallback func(text string, entryType EntryType)

// ChatSession orchestrates a single chat session with tool-calling loop.
type ChatSession struct {
	cfg               config.Config
	model             string
	planBoard         *plan.PlanBoard
	client            *client.Client
	conversation      *Conversation
	tools             *tools.Registry
	outputCb          OutputCallback
	lastUsage         Usage
	contextLimitCache sync.Map // key: "url:model", value: int
}

// NewSession creates a new chat session with the given configuration and plan board.
func NewSession(cfg config.Config, pb *plan.PlanBoard) *ChatSession {
	s := &ChatSession{
		cfg:          cfg,
		model:        cfg.Model,
		planBoard:    pb,
		client:       client.New(cfg.APIBase, cfg.APIKey),
		conversation: NewConversation(cfg.SystemPrompt),
		tools:        tools.NewRegistry(),
	}
	// Add default tools
	s.tools.AddDefaults(cfg.SafeDir, cfg.WorktreeBase, cfg.SearchAPIKey, cfg.SearchEngineID, cfg.SearchEndpoint)
	// Add plan tools
	s.tools.Add(tools.MakeWritePlanTool(pb))
	s.tools.Add(tools.MakeReadPlanTool(pb))
	s.tools.Add(tools.MakeCommentPlanTool(pb))

	// Wire conversation compaction to summarize via LLM
	s.conversation.SetSummaryCallback(func(messages []Message, maxTokens int) *string {
		return s.summarizeMessages(messages, maxTokens)
	})

	return s
}

// SetModel changes the model used by this session.
func (s *ChatSession) SetModel(model string) {
	s.model = model
}

// Model returns the current model name.
func (s *ChatSession) Model() string {
	return s.model
}

// Clear resets the conversation history.
func (s *ChatSession) Clear() {
	s.conversation.Clear()
}

// Compact manually triggers conversation compaction.
func (s *ChatSession) Compact() {
	s.conversation.Compact()
}

// PlanBoard returns the session's plan board.
func (s *ChatSession) PlanBoard() *plan.PlanBoard {
	return s.planBoard
}

// SetOutputCallback sets a callback for streaming output updates.
func (s *ChatSession) SetOutputCallback(cb OutputCallback) {
	s.outputCb = cb
}

// LastUsage returns the token usage from the last API call.
func (s *ChatSession) LastUsage() Usage {
	return s.lastUsage
}

// SafeDir returns the configured safe directory.
func (s *ChatSession) SafeDir() string {
	return s.cfg.SafeDir
}

// ClientForModels returns the underlying HTTP client (for model discovery).
func (s *ChatSession) ClientForModels() *client.Client {
	return s.client
}

// discoverContextLimit returns the context window for the current model.
// It caches results per {client.URL}:{model} in a sync.Map.
// Falls back to cfg.ContextLimit if discovery fails or config has an explicit value.
func (s *ChatSession) discoverContextLimit(ctx context.Context) int {
	// If config has an explicit ContextLimit (non-zero), prefer it
	if s.cfg.ContextLimit > 0 {
		return s.cfg.ContextLimit
	}

	// Check cache
	key := s.client.URL() + ":" + s.model
	if val, ok := s.contextLimitCache.Load(key); ok {
		if limit, ok := val.(int); ok && limit > 0 {
			return limit
		}
	}

	// Fetch from API
	limit, err := s.client.FetchModelContextLimit(ctx, s.model)
	if err == nil && limit > 0 {
		s.contextLimitCache.Store(key, limit)
		return limit
	}

	// Fall back to config default (which may be 0 or default value)
	return s.cfg.ContextLimit
}

// RunOnce executes a single user turn.
// It sends the user's message to the LLM, processes any tool calls iteratively,
// and returns the final assistant response.
func (s *ChatSession) RunOnce(ctx context.Context, userInput string) (*ChatResult, error) {
	// Save snapshot for error recovery
	snapshot := s.conversation.Size()

	// Discover context limit for this model (cached per {url}:{model})
	contextLimit := s.discoverContextLimit(ctx)

	// Add user message
	s.conversation.AddUser(userInput)

	for iteration := 0; iteration < s.cfg.MaxToolIterations; iteration++ {
		// Compact if needed
		if s.conversation.NeedsCompaction(contextLimit, s.cfg.CompactThreshold) {
			s.conversation.Compact()
			if s.outputCb != nil {
				s.outputCb("[⌂ compaction]", EntryToolCall)
			}
		}

		// Build the API payload
		payload := map[string]any{
			"model":            s.model,
			"messages":         s.conversation.ToOpenAIMessages(),
			"tools":            s.tools.ToOpenAITools(),
			"stream":           true,
		}

		// Streaming callbacks
		var contentBuilder strings.Builder
		var reasoningBuilder strings.Builder
		var toolAcc ToolAccumulator
		streamErrored := false
		var streamErrMsg string

		err := s.client.StreamChat(ctx, payload, client.SSECallbacks{
			OnData: func(data map[string]any) {
				// Capture usage if present
				if usageRaw, ok := data["usage"]; ok {
					if usageMap, ok := usageRaw.(map[string]any); ok {
						s.lastUsage.FromJSON(usageMap)
					}
				}

				choices, ok := data["choices"].([]any)
				if !ok || len(choices) == 0 {
					return
				}
				choice, ok := choices[0].(map[string]any)
				if !ok {
					return
				}
				delta, ok := choice["delta"].(map[string]any)
				if !ok {
					return
				}

				// Reasoning content
				if rc, ok := delta["reasoning_content"].(string); ok {
					reasoningBuilder.WriteString(rc)
					if s.outputCb != nil {
						s.outputCb(rc, EntryReasoning)
					}
				}

				// Tool calls
				if tcRaw, ok := delta["tool_calls"]; ok {
					if tcArr, ok := tcRaw.([]any); ok {
						for _, tcItem := range tcArr {
							if tcMap, ok := tcItem.(map[string]any); ok {
								toolAcc.Apply(map[string]any{"tool_calls": []any{tcMap}})
							}
						}
					}
				}

				// Content
				if content, ok := delta["content"].(string); ok {
					contentBuilder.WriteString(content)
					if s.outputCb != nil {
						s.outputCb(content, EntryContent)
					}
				}
			},
			OnDone: func() {
				// Stream completed normally
			},
			OnError: func(err string) {
				streamErrored = true
				streamErrMsg = err
			},
		})

		if err != nil {
			s.conversation.Truncate(snapshot)
			// Provide user-friendly error messages
			errMsg := friendlyError(err, s.cfg.APIBase)
			return nil, errors.New(errMsg)
		}

		if streamErrored && contentBuilder.Len() == 0 {
			s.conversation.Truncate(snapshot)
			errMsg := friendlyError(errors.New(streamErrMsg), s.cfg.APIBase)
			return nil, errors.New(errMsg)
		}

		// Get accumulated tool calls
		calls := toolAcc.Finalize()

		if len(calls) == 0 {
			// No tool calls — this is the final answer
			content := contentBuilder.String()
			reasoning := reasoningBuilder.String()
			s.conversation.AddAssistant(content, reasoning, nil)
			return &ChatResult{Content: content, Reasoning: reasoning}, nil
		}

		// There are tool calls — add assistant message with tool calls
		s.conversation.AddAssistant("", reasoningBuilder.String(), calls)

		// Check cancellation before executing tools
		if err := ctx.Err(); err != nil {
			s.conversation.Truncate(snapshot)
			return nil, fmt.Errorf("interrupted: %w", err)
		}

		// Determine if any tool in this batch has Write permission
		writeToolNames := make(map[string]bool)
		for _, name := range s.tools.ToolNamesByPermission(tools.PermissionWrite) {
			writeToolNames[name] = true
		}

		hasWrite := false
		for _, call := range calls {
			if writeToolNames[call.Name] {
				hasWrite = true
				break
			}
		}

		if hasWrite || len(calls) == 1 {
			// Serial execution
			for _, call := range calls {
				if err := ctx.Err(); err != nil {
					s.conversation.Truncate(snapshot)
					return nil, fmt.Errorf("interrupted: %w", err)
				}
				if s.outputCb != nil {
					s.outputCb(fmt.Sprintf("→ %s(%s)", call.Name, call.Arguments), EntryToolCall)
				}
				result, execErr := s.tools.Execute(ctx, call.Name, call.Arguments)
				resultStr := result
				if execErr != nil {
					resultStr = execErr.Error()
				}
				s.conversation.AddTool(call.ID, resultStr)
			}
		} else {
			// Parallel execution for read-only tools
			type toolResult struct {
				id     string
				result string
			}
			ch := make(chan toolResult, len(calls))

			var wg sync.WaitGroup
			for _, call := range calls {
				wg.Add(1)
				call := call // capture
				go func() {
					defer wg.Done()
					if s.outputCb != nil {
						s.outputCb(fmt.Sprintf("→ %s(%s)", call.Name, call.Arguments), EntryToolCall)
					}
					result, execErr := s.tools.Execute(ctx, call.Name, call.Arguments)
					resultStr := result
					if execErr != nil {
						resultStr = execErr.Error()
					}
					ch <- toolResult{call.ID, resultStr}
				}()
			}
			wg.Wait()
			close(ch)

			for tr := range ch {
				s.conversation.AddTool(tr.id, tr.result)
			}
		}

		// Continue the loop — the AI will process tool results
	}

	// Max iterations reached
	s.conversation.Truncate(snapshot)
	return nil, fmt.Errorf("Maximum tool call iterations (%d) reached. Increase via LLM_MAX_TOOL_ITERATIONS env var.", s.cfg.MaxToolIterations)
}

// friendlyError converts common error patterns to user-friendly messages.
func friendlyError(err error, apiBase string) string {
	msg := err.Error()

	// Connection refused / unreachable
	if strings.Contains(msg, "connection refused") {
		return fmt.Sprintf("Cannot connect to %s. Is the server running?", apiBase)
	}
	if strings.Contains(msg, "no such host") || strings.Contains(msg, "lookup") {
		return fmt.Sprintf("Cannot resolve %s. Check the LLM_API setting.", apiBase)
	}
	if strings.Contains(msg, "timeout") || strings.Contains(msg, "deadline exceeded") {
		return "Request timed out. The model may be overloaded."
	}

	// HTTP-level errors
	if strings.Contains(msg, "HTTP 401") || strings.Contains(msg, "HTTP 403") {
		return "Authentication failed. Check your API key."
	}
	if strings.Contains(msg, "HTTP 429") {
		return "Rate limited. Please wait before sending another request."
	}
	if strings.Contains(msg, "HTTP 5") {
		return fmt.Sprintf("Server error at %s. The model may be overloaded.", apiBase)
	}

	// Context cancellation
	if strings.Contains(msg, "context canceled") || strings.Contains(msg, "interrupted") {
		return "Chat was cancelled."
	}

	return msg
}

// summarizeMessages calls the LLM to summarize a set of messages for compaction.
func (s *ChatSession) summarizeMessages(messages []Message, maxTokens int) *string {
	summaryConv := NewConversation(
		"Summarize the following conversation exchanges concisely. " +
			"Preserve the user's intent, key decisions, and any information " +
			"that will be needed to continue the task. " +
			"Output only the summary, no preamble.")

	for _, msg := range messages {
		if msg.Role == "user" && msg.Content != nil {
			summaryConv.AddUser(*msg.Content)
		} else if msg.Role == "assistant" && msg.Content != nil {
			summaryConv.AddAssistant(*msg.Content, "", nil)
		} else if msg.Role == "assistant" && len(msg.ToolCalls) > 0 {
			var parts []string
			for _, tc := range msg.ToolCalls {
				parts = append(parts, tc.Name+"("+tc.Arguments+")")
			}
			summaryConv.AddAssistant("[called tools: "+strings.Join(parts, ", ")+"]", "", nil)
		} else if msg.Role == "tool" && msg.Content != nil {
			summaryConv.AddTool(msg.ToolCallID, fmt.Sprintf("[tool result: %d bytes]", len(*msg.Content)))
		}
	}

	// Set a low max_tokens for the summary request
	summaryTokens := maxTokens
	if summaryTokens > 1024 {
		summaryTokens = 1024
	}

	payload := map[string]any{
		"model":     s.model,
		"messages":  summaryConv.ToOpenAIMessages(),
		"stream":    false,
		"max_tokens": summaryTokens,
	}

	result, err := s.client.Chat(context.Background(), payload)
	if err != nil {
		log.Printf("summarizeMessages: chat error: %v", err)
		return nil
	}

	choices, ok := result["choices"].([]any)
	if !ok || len(choices) == 0 {
		return nil
	}
	choice, ok := choices[0].(map[string]any)
	if !ok {
		return nil
	}
	message, ok := choice["message"].(map[string]any)
	if !ok {
		return nil
	}
	content, ok := message["content"].(string)
	if !ok || content == "" {
		return nil
	}

	return &content
}
