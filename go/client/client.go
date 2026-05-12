package client

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math"
	"math/rand"
	"net/http"
	"strings"
	"time"
)

const (
	maxRetries     = 3
	baseDelaySec   = 1.0
	userAgent      = "cima/0.1"
	connectTimeout = 10 * time.Second
	chatTimeout    = 3600 * time.Second // 1 hour for streaming
	queryTimeout   = 30 * time.Second
)

// Client is an OpenAI-compatible HTTP client for chat completions.
type Client struct {
	apiBase       string
	apiKey        string
	httpClient    *http.Client
	lastRawBody   string
}

// New creates a new Client with the given API base URL and optional API key.
// Trailing slashes on apiBase are preserved (they're harmless for path joining).
func New(apiBase, apiKey string) *Client {
	// Strip trailing slashes for consistent URL construction
	apiBase = strings.TrimRight(apiBase, "/")

	return &Client{
		apiBase: apiBase,
		apiKey:  apiKey,
		httpClient: &http.Client{
			Timeout: chatTimeout,
			Transport: &http.Transport{
				ResponseHeaderTimeout: connectTimeout,
			},
		},
	}
}

// URL returns the full chat completions endpoint URL.
func (c *Client) URL() string {
	return c.apiBase + "/chat/completions"
}

// ModelsURL returns the full models endpoint URL.
func (c *Client) ModelsURL() string {
	return c.apiBase + "/models"
}

// LastRawResponse returns the raw body of the last response (for debugging).
func (c *Client) LastRawResponse() string {
	return c.lastRawBody
}

// Chat sends a non-streaming request and returns the parsed JSON response.
func (c *Client) Chat(ctx context.Context, payload map[string]any) (map[string]any, error) {
	body, err := c.doRequest(ctx, c.URL(), payload, false)
	if err != nil {
		return nil, err
	}
	return body, nil
}

// StreamChat sends a streaming request, calling SSEParser callbacks as data arrives.
func (c *Client) StreamChat(ctx context.Context, payload map[string]any, cb SSECallbacks) error {
	// Set stream flag
	payload["stream"] = true

	payloadBytes, err := json.Marshal(payload)
	if err != nil {
		return fmt.Errorf("marshal payload: %w", err)
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.URL(), bytes.NewReader(payloadBytes))
	if err != nil {
		return fmt.Errorf("create request: %w", err)
	}
	c.setHeaders(req)

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return fmt.Errorf("request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		bodyBytes, _ := io.ReadAll(resp.Body)
		c.lastRawBody = string(bodyBytes)
		return fmt.Errorf("HTTP %d: %s", resp.StatusCode, truncate(string(bodyBytes), 500))
	}

	parser := NewSSEParser(cb)

	// Read the streaming body in chunks
	buf := make([]byte, 4096)
	for {
		n, readErr := resp.Body.Read(buf)
		if n > 0 {
			parser.Feed(string(buf[:n]))
		}
		if readErr == io.EOF {
			parser.Flush()
			break
		}
		if readErr != nil {
			parser.Flush()
			return fmt.Errorf("read stream: %w", readErr)
		}
	}

	return nil
}

// FetchModels queries /v1/models and returns the list of model IDs.
func (c *Client) FetchModels(ctx context.Context) ([]string, error) {
	body, err := c.doRequest(ctx, c.ModelsURL(), nil, true)
	if err != nil {
		return nil, err
	}

	dataRaw, ok := body["data"]
	if !ok {
		return nil, fmt.Errorf("response missing 'data' field")
	}
	dataArr, ok := dataRaw.([]any)
	if !ok {
		return nil, fmt.Errorf("'data' is not an array")
	}

	var models []string
	for _, item := range dataArr {
		itemMap, ok := item.(map[string]any)
		if !ok {
			continue
		}
		id, ok := itemMap["id"].(string)
		if !ok || id == "" {
			continue
		}
		models = append(models, id)
	}
	return models, nil
}

// FetchModelContextLimit tries to discover the context window for a model.
// Returns the discovered limit, or 0 if discovery fails.
func (c *Client) FetchModelContextLimit(ctx context.Context, model string) (int, error) {
	body, err := c.doRequest(ctx, c.ModelsURL(), nil, true)
	if err != nil {
		return 0, nil // non-fatal: caller should fall back to default
	}

	// Find the model object
	var modelObj map[string]any

	dataRaw, ok := body["data"]
	if ok {
		if dataArr, ok := dataRaw.([]any); ok {
			for _, item := range dataArr {
				if itemMap, ok := item.(map[string]any); ok {
					id, _ := itemMap["id"].(string)
					name, _ := itemMap["name"].(string)
					if id == model || name == model {
						modelObj = itemMap
						break
					}
				}
			}
			// Fall back to first model if specific not found
			if modelObj == nil && len(dataArr) > 0 {
				modelObj, _ = dataArr[0].(map[string]any)
			}
		}
	} else {
		// Single model object
		if id, _ := body["id"].(string); id == model {
			modelObj = body
		}
	}

	if modelObj == nil {
		return 0, nil
	}

	// Known field names across backends, in priority order
	contextFields := []string{
		"context_window",
		"max_model_len",
		"max_context_length",
		"context_length",
		"inputTokenLimit",
		"max_input_tokens",
		"max_total_tokens",
	}

	for _, field := range contextFields {
		if v, ok := modelObj[field]; ok {
			switch val := v.(type) {
			case float64:
				if int(val) > 0 {
					return int(val), nil
				}
			case int64:
				if int(val) > 0 {
					return int(val), nil
				}
			case int:
				if val > 0 {
					return val, nil
				}
			}
		}
	}

	return 0, nil
}

// doRequest performs an HTTP request with retry logic.
// If payload is nil and isGet is true, performs a GET request.
func (c *Client) doRequest(ctx context.Context, url string, payload map[string]any, isGet bool) (map[string]any, error) {
	var lastErr error

	for attempt := 0; attempt < maxRetries; attempt++ {
		// Check context cancellation before each attempt
		if err := ctx.Err(); err != nil {
			return nil, err
		}

		var req *http.Request
		var err error

		if isGet || payload == nil {
			req, err = http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
		} else {
			var payloadBytes []byte
			payloadBytes, err = json.Marshal(payload)
			if err != nil {
				return nil, fmt.Errorf("marshal payload: %w", err)
			}
			req, err = http.NewRequestWithContext(ctx, http.MethodPost, url, bytes.NewReader(payloadBytes))
		}
		if err != nil {
			return nil, fmt.Errorf("create request: %w", err)
		}
		c.setHeaders(req)

		resp, doErr := c.httpClient.Do(req)
		if doErr != nil {
			lastErr = doErr
			if attempt < maxRetries-1 && isRetryableError(doErr) {
				// Also check context before sleeping for retry
				if err := ctx.Err(); err != nil {
					return nil, err
				}
				select {
				case <-ctx.Done():
					return nil, ctx.Err()
				case <-time.After(jitteredDelay(baseDelaySec * math.Pow(2, float64(attempt)))):
				}
				continue
			}
			return nil, doErr
		}

		bodyBytes, readErr := io.ReadAll(resp.Body)
		resp.Body.Close()
		c.lastRawBody = string(bodyBytes)

		if readErr != nil {
			lastErr = readErr
			if attempt < maxRetries-1 && isRetryableError(readErr) {
				select {
				case <-ctx.Done():
					return nil, ctx.Err()
				case <-time.After(jitteredDelay(baseDelaySec * math.Pow(2, float64(attempt)))):
				}
				continue
			}
			return nil, readErr
		}

		if resp.StatusCode == http.StatusOK {
			var result map[string]any
			if err := json.Unmarshal(bodyBytes, &result); err != nil {
				return nil, fmt.Errorf("JSON parse error: %w", err)
			}
			return result, nil
		}

		lastErr = fmt.Errorf("HTTP %d: %s", resp.StatusCode, truncate(string(bodyBytes), 500))

		if attempt < maxRetries-1 && shouldRetry(resp.StatusCode) {
			select {
			case <-ctx.Done():
				return nil, ctx.Err()
			case <-time.After(jitteredDelay(baseDelaySec * math.Pow(2, float64(attempt)))):
			}
			continue
		}

		return nil, lastErr
	}

	return nil, lastErr
}

// setHeaders sets common HTTP headers on the request.
func (c *Client) setHeaders(req *http.Request) {
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept-Encoding", "gzip")
	if c.apiKey != "" {
		req.Header.Set("Authorization", "Bearer "+c.apiKey)
	}
}

// shouldRetry returns true if the HTTP status code indicates a retryable error.
func shouldRetry(statusCode int) bool {
	return statusCode == http.StatusTooManyRequests || (statusCode >= 500 && statusCode < 600)
}

// isRetryableError returns true if the error is a transient network error.
func isRetryableError(err error) bool {
	s := err.Error()
	return strings.Contains(s, "timeout") ||
		strings.Contains(s, "connection") ||
		strings.Contains(s, "reset") ||
		strings.Contains(s, "refused")
}

// jitteredDelay returns a random delay in [0.5*base, 1.5*base].
func jitteredDelay(base float64) time.Duration {
	jitter := 0.5 + rand.Float64() // [0.5, 1.5]
	return time.Duration(base * jitter * float64(time.Second))
}

// truncate truncates a string to maxLen characters, appending "..." if truncated.
func truncate(s string, maxLen int) string {
	if len(s) <= maxLen {
		return s
	}
	return s[:maxLen] + "..."
}
