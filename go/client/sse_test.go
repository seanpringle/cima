package client

import (
	"encoding/json"
	"testing"
)

func decodeMap(t *testing.T, data string) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal([]byte(data), &m); err != nil {
		t.Fatalf("failed to decode %q: %v", data, err)
	}
	return m
}

// ── Tests for SSEParser ──

func TestParseSingleCompleteEvent(t *testing.T) {
	var received []map[string]any
	done := false

	p := NewSSEParser(SSECallbacks{
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
	p.Feed("data: {\"key\":\"value\"}\n\n")

	if len(received) != 1 {
		t.Fatalf("expected 1 data event, got %d", len(received))
	}
	if received[0]["key"] != "value" {
		t.Errorf("key = %v, want 'value'", received[0]["key"])
	}
	if done {
		t.Error("done should not be set yet")
	}
}

func TestParseMultipleEvents(t *testing.T) {
	var received []map[string]any
	done := false

	p := NewSSEParser(SSECallbacks{
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
	p.Feed("data: {\"a\":1}\n\ndata: {\"b\":2}\n\ndata: [DONE]\n\n")

	if len(received) != 2 {
		t.Fatalf("expected 2 data events, got %d", len(received))
	}
	if received[0]["a"] != float64(1) {
		t.Errorf("first data a = %v, want 1", received[0]["a"])
	}
	if received[1]["b"] != float64(2) {
		t.Errorf("second data b = %v, want 2", received[1]["b"])
	}
	if !done {
		t.Error("done should be set")
	}
}

func TestParsePartialAcrossFeeds(t *testing.T) {
	var received []map[string]any

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("data: {\"k")
	if len(received) != 0 {
		t.Fatal("expected no data from incomplete feed")
	}

	p.Feed("ey\":1}\n\n")
	if len(received) != 1 {
		t.Fatalf("expected 1 data event after second feed, got %d", len(received))
	}
	if received[0]["key"] != float64(1) {
		t.Errorf("key = %v, want 1", received[0]["key"])
	}
}

func TestParseIgnoresNonDataLines(t *testing.T) {
	var received []map[string]any

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("event: test\ndata: {\"x\":1}\n\n")
	if len(received) != 1 {
		t.Fatalf("expected 1 data event, got %d", len(received))
	}
	if received[0]["x"] != float64(1) {
		t.Errorf("x = %v, want 1", received[0]["x"])
	}
}

func TestParseDoneSignal(t *testing.T) {
	done := false

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {},
		OnDone: func() {
			done = true
		},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("data: [DONE]\n\n")
	if !done {
		t.Error("OnDone should have been called")
	}
}

func TestParseMalformedJSON(t *testing.T) {
	var errMsg string

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {},
		OnDone: func() {},
		OnError: func(err string) {
			errMsg = err
		},
	})

	p.Feed("data: {invalid}\n\n")
	if errMsg == "" {
		t.Fatal("expected error for malformed JSON, got none")
	}
}

func TestFlushBufferedData(t *testing.T) {
	var received []map[string]any
	done := false

	p := NewSSEParser(SSECallbacks{
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

	// Feed partial: first event complete, [DONE] lacks trailing \n
	p.Feed("data: {\"key\":\"value\"}\n\ndata: [DONE]")
	if len(received) != 1 {
		t.Fatalf("expected 1 data event from complete line, got %d", len(received))
	}
	if done {
		t.Error("done should not be set until flush")
	}

	p.Flush()
	if !done {
		t.Error("OnDone should have been called after flush")
	}
}

func TestFlushWithNoBuffered(t *testing.T) {
	p := NewSSEParser(SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	// Should not panic
	p.Flush()
	p.Feed("data: {\"x\":1}\n\n")
	p.Flush()
	p.Reset()
	p.Flush()
}

func TestResetClearsBuffer(t *testing.T) {
	var received []map[string]any

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("data: {\"a\":1}") // incomplete, stays buffered
	p.Reset()
	p.Feed("data: {\"b\":2}\n\n")

	if len(received) != 1 {
		t.Fatalf("expected 1 data event after reset, got %d", len(received))
	}
	if received[0]["b"] != float64(2) {
		t.Errorf("b = %v, want 2", received[0]["b"])
	}
}

func TestRawAccumulation(t *testing.T) {
	p := NewSSEParser(SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	p.Feed("data: {\"a\":1}\n\n")
	p.Feed("data: {\"b\":2}\n\n")
	raw := p.Raw()
	if raw != "data: {\"a\":1}\n\ndata: {\"b\":2}\n\n" {
		t.Errorf("Raw() = %q, want %q", raw, "data: {\"a\":1}\n\ndata: {\"b\":2}\n\n")
	}
}

func TestFeedWithCarriageReturn(t *testing.T) {
	var received []map[string]any

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("data: {\"a\":1}\r\n\r\n")
	if len(received) != 1 {
		t.Fatalf("expected 1 data event, got %d", len(received))
	}
	if received[0]["a"] != float64(1) {
		t.Errorf("a = %v, want 1", received[0]["a"])
	}
}

func TestEmptyFeed(t *testing.T) {
	p := NewSSEParser(SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	p.Feed("") // Should not panic
	p.Flush()
}

func TestFeedKeepsBufferingPartialLineAcrossMultipleCalls(t *testing.T) {
	var received []map[string]any

	p := NewSSEParser(SSECallbacks{
		OnData: func(data map[string]any) {
			received = append(received, data)
		},
		OnDone: func() {},
		OnError: func(err string) {
			t.Fatalf("unexpected error: %s", err)
		},
	})

	p.Feed("dat")
	p.Feed("a: {\"x\":1}")
	p.Feed("\n")
	p.Feed("\n")

	if len(received) != 1 {
		t.Fatalf("expected 1 data event, got %d", len(received))
	}
	if received[0]["x"] != float64(1) {
		t.Errorf("x = %v, want 1", received[0]["x"])
	}
}

func TestRawAfterReset(t *testing.T) {
	p := NewSSEParser(SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {},
	})
	p.Feed("data: {\"a\":1}\n\n")
	p.Reset()
	raw := p.Raw()
	if raw != "" {
		t.Errorf("Raw() after reset = %q, want empty", raw)
	}
}

func TestOnErrorSwallowsPanic(t *testing.T) {
	// Verify that malformed JSON that causes a panic in json.Unmarshal is caught
	// and delivered to OnError instead of crashing.
	var errMsg string
	p := NewSSEParser(SSECallbacks{
		OnData:  func(data map[string]any) {},
		OnDone:  func() {},
		OnError: func(err string) {
			errMsg = err
		},
	})
	p.Feed("data: \n\n") // empty data after prefix
	if errMsg == "" {
		t.Log("empty data may or may not produce an error; non-empty body expected")
	}

	// Feed a very broken line
	errMsg = ""
	p.Feed("data: \x00\x01\x02\n\n")
	if errMsg == "" {
		t.Error("expected error for binary garbage in JSON")
	}
}
