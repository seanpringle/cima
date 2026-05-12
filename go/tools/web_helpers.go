package tools

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"
)

// ── Shared HTTP GET helper ──

// httpGet performs a GET request with the given timeout and cancellation support.
// Returns (body, statusCode) or error.
var httpGet = func(ctx context.Context, url string, timeoutSec int) (string, int, error) {
	reqCtx, cancel := context.WithTimeout(ctx, time.Duration(timeoutSec)*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(reqCtx, "GET", url, nil)
	if err != nil {
		return "", 0, fmt.Errorf("create request: %w", err)
	}
	req.Header.Set("User-Agent", "cima/0.1")

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return "", 0, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return "", resp.StatusCode, err
	}

	return string(body), resp.StatusCode, nil
}

// ── URL validation ──

func isValidFetchScheme(url string) bool {
	pos := strings.Index(url, ":")
	if pos < 0 {
		return false
	}
	scheme := strings.ToLower(url[:pos])
	return scheme == "http" || scheme == "https"
}

// ── DuckDuckGo rate limiter ──

var (
	lastDDGRequest time.Time
	ddgMutex       sync.Mutex
	ddgMinInterval = 1 * time.Second
)

// rateLimitDDG blocks until the minimum interval since the last DDG request has elapsed.
func rateLimitDDG() {
	ddgMutex.Lock()
	defer ddgMutex.Unlock()
	elapsed := time.Since(lastDDGRequest)
	if elapsed < ddgMinInterval {
		time.Sleep(ddgMinInterval - elapsed)
	}
	lastDDGRequest = time.Now()
}

// ── Caching for web_fetch ──

var (
	fetchCache     = make(map[string]string)
	fetchCacheLock sync.Mutex
)
