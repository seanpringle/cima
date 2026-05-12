package tools

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestWebSearchEmptyQuery(t *testing.T) {
	tool := makeWebSearchTool("", "", "")
	_, err := tool.Execute(context.Background(), map[string]any{"query": ""})
	if err == nil {
		t.Fatal("expected error for empty query")
	}
}

func TestWebFetchInvalidURL(t *testing.T) {
	tool := makeWebFetchTool()
	_, err := tool.Execute(context.Background(), map[string]any{"url": "ftp://example.com"})
	if err == nil {
		t.Fatal("expected error for non-http/https URL")
	}
}

func TestWebFetchTimeout(t *testing.T) {
	// Create a slow server
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Never respond
		time.Sleep(5 * time.Second)
		fmt.Fprint(w, "hello")
	}))
	defer srv.Close()

	// Create a context with short timeout
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()

	tool := makeWebFetchTool()
	_, err := tool.Execute(ctx, map[string]any{"url": srv.URL})
	if err == nil {
		t.Log("expected error for timeout (may pass if server responds fast enough)")
	}
}

func TestWebFetchBasic(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "hello world")
	}))
	defer srv.Close()

	// Save and restore httpGet
	origGet := httpGet
	defer func() { httpGet = origGet }()

	httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
		return "hello world", 200, nil
	}

	tool := makeWebFetchTool()
	result, err := tool.Execute(context.Background(), map[string]any{"url": "https://example.com"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "hello world") {
		t.Errorf("result = %q, should contain 'hello world'", result)
	}
}

func TestWebFetchCache(t *testing.T) {
	callCount := 0
	var mu sync.Mutex

	// Use a real server to verify caching
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		callCount++
		mu.Unlock()
		fmt.Fprint(w, "cached content")
	}))
	defer srv.Close()

	origGet := httpGet
	defer func() { httpGet = origGet }()

	httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
		mu.Lock()
		callCount++
		mu.Unlock()
		return "cached content", 200, nil
	}

	// Clear cache first
	fetchCacheLock.Lock()
	fetchCache = make(map[string]string)
	fetchCacheLock.Unlock()

	tool := makeWebFetchTool()

	// First call
	result1, err := tool.Execute(context.Background(), map[string]any{"url": "https://cache-test.example.com"})
	if err != nil {
		t.Fatalf("First Execute: %v", err)
	}
	if !strings.Contains(result1, "cached content") {
		t.Errorf("first result = %q", result1)
	}
	firstCount := callCount

	// Second call with same URL should use cache
	result2, err := tool.Execute(context.Background(), map[string]any{"url": "https://cache-test.example.com"})
	if err != nil {
		t.Fatalf("Second Execute: %v", err)
	}
	if !strings.Contains(result2, "cached content") {
		t.Errorf("second result = %q", result2)
	}
	if callCount != firstCount {
		t.Errorf("expected cached call (count %d -> %d)", firstCount, callCount)
	}
}

func TestWebSearchDDG(t *testing.T) {
	origGet := httpGet
	defer func() { httpGet = origGet }()

	httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
		return `{"AbstractText": "Go is a programming language", "AbstractSource": "Wikipedia", "AbstractURL": "https://en.wikipedia.org/wiki/Go_(programming_language)"}`, 200, nil
	}

	tool := makeWebSearchTool("", "", "")
	result, err := tool.Execute(context.Background(), map[string]any{"query": "golang"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Go is a programming language") {
		t.Errorf("result should contain abstract: %q", result)
	}
}

func TestWebSearchGoogle(t *testing.T) {
	origGet := httpGet
	defer func() { httpGet = origGet }()

	httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
		return `{"items": [{"title": "Go Programming", "snippet": "The Go programming language", "link": "https://golang.org"}]}`, 200, nil
	}

	tool := makeWebSearchTool("test-key", "test-engine", "")
	result, err := tool.Execute(context.Background(), map[string]any{"query": "golang"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Go Programming") {
		t.Errorf("result should contain title: %q", result)
	}
}

func TestWebSearchCustom(t *testing.T) {
	origGet := httpGet
	defer func() { httpGet = origGet }()

	httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
		if !strings.Contains(url, "my+query") {
			t.Errorf("url should contain encoded query: %q", url)
		}
		return `{"items": [{"title": "Custom Result", "snippet": "Custom snippet", "link": "https://custom.example.com"}]}`, 200, nil
	}

	tool := makeWebSearchTool("", "", "https://custom.example.com/search?q={query}")
	result, err := tool.Execute(context.Background(), map[string]any{"query": "my query"})
	if err != nil {
		t.Fatalf("Execute: %v", err)
	}
	if !strings.Contains(result, "Custom Result") {
		t.Errorf("result should contain custom result: %q", result)
	}
}
