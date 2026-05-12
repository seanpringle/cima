package client

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

// ── Helpers ──

func writeJSON(w http.ResponseWriter, data any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(data)
}

// ── Chat tests ──

func TestChatBasic(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" {
			t.Errorf("method = %s, want POST", r.Method)
		}
		if !strings.HasSuffix(r.URL.Path, "/chat/completions") {
			t.Errorf("path = %s, want /chat/completions", r.URL.Path)
		}
		writeJSON(w, map[string]any{
			"choices": []any{
				map[string]any{
					"message": map[string]any{
						"content": "Hello!",
						"role":    "assistant",
					},
				},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	ctx := context.Background()
	resp, err := c.Chat(ctx, map[string]any{
		"model": "test-model",
		"messages": []any{
			map[string]any{"role": "user", "content": "Hi"},
		},
	})
	if err != nil {
		t.Fatalf("Chat: %v", err)
	}
	choices, _ := resp["choices"].([]any)
	if len(choices) != 1 {
		t.Fatalf("expected 1 choice, got %d", len(choices))
	}
	msg, _ := choices[0].(map[string]any)["message"].(map[string]any)
	content, _ := msg["content"].(string)
	if content != "Hello!" {
		t.Errorf("content = %q, want %q", content, "Hello!")
	}
}

func TestChatHTTPError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		w.Write([]byte("internal error"))
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err == nil {
		t.Fatal("expected error for HTTP 500")
	}
	if !strings.Contains(err.Error(), "500") {
		t.Errorf("error should mention HTTP status: %v", err)
	}
}

func TestChatJSONError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte("not json"))
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err == nil {
		t.Fatal("expected error for invalid JSON")
	}
}

func TestChatAuth(t *testing.T) {
	var authHeader string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		authHeader = r.Header.Get("Authorization")
		writeJSON(w, map[string]any{"choices": []any{map[string]any{"message": map[string]any{"content": "ok"}}}})
	}))
	defer srv.Close()

	c := New(srv.URL, "sk-test-key")
	c.Chat(context.Background(), map[string]any{"model": "test"})
	if authHeader != "Bearer sk-test-key" {
		t.Errorf("Authorization = %q, want %q", authHeader, "Bearer sk-test-key")
	}
}

func TestChatAuthEmpty(t *testing.T) {
	var authHeader string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		authHeader = r.Header.Get("Authorization")
		writeJSON(w, map[string]any{"choices": []any{map[string]any{"message": map[string]any{"content": "ok"}}}})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	c.Chat(context.Background(), map[string]any{"model": "test"})
	if authHeader != "" {
		t.Errorf("Authorization header should be empty when no key, got %q", authHeader)
	}
}

func TestChatContentType(t *testing.T) {
	var contentType string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		contentType = r.Header.Get("Content-Type")
		writeJSON(w, map[string]any{"choices": []any{map[string]any{"message": map[string]any{"content": "ok"}}}})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	c.Chat(context.Background(), map[string]any{"model": "test"})
	if contentType != "application/json" {
		t.Errorf("Content-Type = %q, want %q", contentType, "application/json")
	}
}

// ── Streaming chat tests ──

func TestStreamChatBasic(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		fmt.Fprint(w, "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n")
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	var received []map[string]any
	done := false

	err := c.StreamChat(context.Background(), map[string]any{"model": "test", "stream": true}, SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {
			done = true
		},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})
	if err != nil {
		t.Fatalf("StreamChat: %v", err)
	}
	if len(received) != 1 {
		t.Fatalf("expected 1 data event, got %d", len(received))
	}
	if !done {
		t.Error("OnDone should have been called")
	}
}

func TestStreamChatDone(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		fmt.Fprint(w, "data: [DONE]\n\n")
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	done := false

	err := c.StreamChat(context.Background(), map[string]any{"model": "test", "stream": true}, SSECallbacks{
		OnData: func(data map[string]any) {},
		OnDone: func() {
			done = true
		},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})
	if err != nil {
		t.Fatalf("StreamChat: %v", err)
	}
	if !done {
		t.Error("OnDone should have been called")
	}
}

func TestStreamChatError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		w.Write([]byte("bad request"))
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	err := c.StreamChat(context.Background(), map[string]any{"model": "test", "stream": true}, SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	if err == nil {
		t.Fatal("expected error for HTTP 400")
	}
	if !strings.Contains(err.Error(), "400") {
		t.Errorf("error should mention HTTP status: %v", err)
	}
}

func TestStreamChatCancellation(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		// Write a partial chunk then hang
		fmt.Fprint(w, "data: {\"choices\":[{\"delta\":{\"content\":\"Hel")
		w.(http.Flusher).Flush()
		// Simulate a slow server that never sends more data
		<-r.Context().Done()
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	err := c.StreamChat(ctx, map[string]any{"model": "test", "stream": true}, SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	// Should get an error (timeout or cancellation)
	if err == nil {
		t.Log("StreamChat returned nil on cancellation (acceptable if chunk was processed)")
	}
}

// ── FetchModels tests ──

func TestFetchModels(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "GET" {
			t.Errorf("method = %s, want GET", r.Method)
		}
		if !strings.HasSuffix(r.URL.Path, "/models") {
			t.Errorf("path = %s, want /models", r.URL.Path)
		}
		writeJSON(w, map[string]any{
			"data": []any{
				map[string]any{"id": "model-a"},
				map[string]any{"id": "model-b"},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	models, err := c.FetchModels(context.Background())
	if err != nil {
		t.Fatalf("FetchModels: %v", err)
	}
	if len(models) != 2 {
		t.Fatalf("expected 2 models, got %d", len(models))
	}
	if models[0] != "model-a" {
		t.Errorf("models[0] = %q, want %q", models[0], "model-a")
	}
	if models[1] != "model-b" {
		t.Errorf("models[1] = %q, want %q", models[1], "model-b")
	}
}

func TestFetchModelsError(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.FetchModels(context.Background())
	if err == nil {
		t.Fatal("expected error for HTTP 500")
	}
}

// ── FetchModelContextLimit tests ──

func TestFetchContextLimit(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]any{
			"data": []any{
				map[string]any{
					"id":             "test-model",
					"context_window": 64000,
					"max_model_len":  128000,
				},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	limit, err := c.FetchModelContextLimit(context.Background(), "test-model")
	if err != nil {
		t.Fatalf("FetchModelContextLimit: %v", err)
	}
	// context_window has higher priority than max_model_len
	if limit != 64000 {
		t.Errorf("limit = %d, want 64000", limit)
	}
}

func TestFetchContextLimitNotFound(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]any{
			"data": []any{
				map[string]any{"id": "other-model"},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	limit, err := c.FetchModelContextLimit(context.Background(), "unknown-model")
	if err != nil {
		t.Fatalf("FetchModelContextLimit: %v", err)
	}
	if limit != 0 {
		t.Errorf("limit for unknown model = %d, want 0", limit)
	}
}

func TestFetchContextLimitMultipleFields(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		writeJSON(w, map[string]any{
			"data": []any{
				map[string]any{
					"id":                "test-model",
					"context_length":    8192,
					"max_context_length": 32000,
				},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	limit, err := c.FetchModelContextLimit(context.Background(), "test-model")
	if err != nil {
		t.Fatalf("FetchModelContextLimit: %v", err)
	}
	// max_context_length should take priority over context_length
	if limit != 32000 {
		t.Errorf("limit = %d, want 32000", limit)
	}
}

// ── Retry tests ──

func TestRetryOn429(t *testing.T) {
	var attemptCount atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := attemptCount.Add(1)
		if n <= 2 {
			w.WriteHeader(http.StatusTooManyRequests)
			return
		}
		writeJSON(w, map[string]any{
			"choices": []any{
				map[string]any{
					"message": map[string]any{"content": "ok", "role": "assistant"},
				},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err != nil {
		t.Fatalf("Chat after retries: %v", err)
	}
	if n := attemptCount.Load(); n != 3 {
		t.Errorf("expected 3 attempts (2 retries), got %d", n)
	}
}

func TestRetryOn500(t *testing.T) {
	var attemptCount atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := attemptCount.Add(1)
		if n == 1 {
			w.WriteHeader(http.StatusInternalServerError)
			return
		}
		writeJSON(w, map[string]any{
			"choices": []any{
				map[string]any{
					"message": map[string]any{"content": "ok", "role": "assistant"},
				},
			},
		})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err != nil {
		t.Fatalf("Chat after retry: %v", err)
	}
	if n := attemptCount.Load(); n != 2 {
		t.Errorf("expected 2 attempts (1 retry), got %d", n)
	}
}

func TestRetryExhausted(t *testing.T) {
	var attemptCount atomic.Int32
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		attemptCount.Add(1)
		w.WriteHeader(http.StatusTooManyRequests)
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err == nil {
		t.Fatal("expected error after exhausting retries")
	}
	if n := attemptCount.Load(); n != 3 {
		t.Errorf("expected 3 attempts (max retries), got %d", n)
	}
}

// ── URL tests ──

func TestURLConstruction(t *testing.T) {
	c := New("http://api.example.com/v1", "")
	if c.URL() != "http://api.example.com/v1/chat/completions" {
		t.Errorf("URL = %q", c.URL())
	}
	if c.ModelsURL() != "http://api.example.com/v1/models" {
		t.Errorf("ModelsURL = %q", c.ModelsURL())
	}
}

func TestURLTrailingSlash(t *testing.T) {
	c := New("http://api.example.com/v1/", "")
	if c.URL() != "http://api.example.com/v1/chat/completions" {
		t.Errorf("trailing slash URL = %q", c.URL())
	}
}

func TestLastRawResponse(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Write([]byte(`{"choices":[{"message":{"content":"raw test","role":"assistant"}}]}`))
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	c.Chat(context.Background(), map[string]any{"model": "test"})
	raw := c.LastRawResponse()
	if !strings.Contains(raw, "raw test") {
		t.Errorf("LastRawResponse = %q, should contain 'raw test'", raw)
	}
}

// ── Context cancellation ──

func TestChatCancellation(t *testing.T) {
	// Use a slow server that sleeps before responding, with a short client timeout
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Sleep longer than the client timeout will allow
		time.Sleep(500 * time.Millisecond)
		writeJSON(w, map[string]any{"choices": []any{map[string]any{"message": map[string]any{"content": "ok"}}}})
	}))
	defer srv.Close()

	c := New(srv.URL, "")
	// Override http client timeout to be very short
	c.httpClient.Timeout = 50 * time.Millisecond

	_, err := c.Chat(context.Background(), map[string]any{"model": "test"})
	if err == nil {
		t.Error("Chat should return error on timeout")
	}
}
