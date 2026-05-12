package client

import (
	"encoding/json"
	"strings"
)

// SSECallbacks are the callbacks invoked by SSEParser when events are parsed.
type SSECallbacks struct {
	// OnData is called for each parsed JSON "data:" line.
	OnData func(data map[string]any)
	// OnDone is called when a "[DONE]" signal is received.
	OnDone func()
	// OnError is called when a JSON parse error occurs in a data line.
	OnError func(err string)
}

// SSEParser incrementally parses Server-Sent Events (SSE) from a stream,
// specifically the OpenAI chat/completions format where each data line
// contains a JSON payload or "[DONE]".
type SSEParser struct {
	cb         SSECallbacks
	buffer     strings.Builder
	raw        strings.Builder
	lineBuffer strings.Builder
}

// NewSSEParser creates a new SSEParser with the given callbacks.
func NewSSEParser(cb SSECallbacks) *SSEParser {
	return &SSEParser{cb: cb}
}

// Feed processes a chunk of raw SSE data. It splits the data into lines
// and processes complete lines. Incomplete lines are buffered until more
// data arrives via a subsequent Feed call or Flush.
func (p *SSEParser) Feed(data string) {
	p.raw.WriteString(data)
	p.buffer.WriteString(data)

	for {
		remaining := p.buffer.String()
		idx := strings.IndexByte(remaining, '\n')
		if idx < 0 {
			return // wait for more data
		}

		line := remaining[:idx]
		p.buffer.Reset()
		p.buffer.WriteString(remaining[idx+1:])

		p.processLine(line)
	}
}

// Flush processes any remaining buffered data as a complete line.
// This is useful when the stream ends without a trailing newline.
func (p *SSEParser) Flush() {
	if p.buffer.Len() == 0 {
		return
	}
	remaining := p.buffer.String()
	p.buffer.Reset()
	p.processLine(strings.TrimRight(remaining, "\r"))
}

// Reset clears the internal buffer and raw data, but preserves the callbacks.
func (p *SSEParser) Reset() {
	p.buffer.Reset()
	p.raw.Reset()
	p.lineBuffer.Reset()
}

// Raw returns the raw SSE text fed since creation or last Reset.
func (p *SSEParser) Raw() string {
	return p.raw.String()
}

// processLine handles a single line of SSE text (without trailing \n).
func (p *SSEParser) processLine(line string) {
	// Strip trailing \r if present (for CRLF line endings)
	line = strings.TrimRight(line, "\r")

	// Skip empty lines (SSE blank line separators between events)
	if line == "" {
		return
	}

	const prefix = "data: "
	if !strings.HasPrefix(line, prefix) {
		// Ignore non-data lines (event:, :keepalive, id:, retry:)
		return
	}

	payload := line[len(prefix):]

	// Check for the stream-end signal
	if payload == "[DONE]" {
		if p.cb.OnDone != nil {
			p.cb.OnDone()
		}
		return
	}

	// Parse JSON payload
	var data map[string]any
	if err := json.Unmarshal([]byte(payload), &data); err != nil {
		if p.cb.OnError != nil {
			p.cb.OnError("SSE error: " + err.Error() + " | payload: " + payload)
		}
		return
	}

	if p.cb.OnData != nil {
		p.cb.OnData(data)
	}
}
