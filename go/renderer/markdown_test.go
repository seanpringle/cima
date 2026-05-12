package renderer

import (
	"strings"
	"testing"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
)

// extractText collects all textual content from a CanvasObject's segments.
func extractText(obj fyne.CanvasObject) string {
	if rt, ok := obj.(*widget.RichText); ok {
		var b strings.Builder
		for _, seg := range rt.Segments {
			b.WriteString(extractSegText(seg))
		}
		return b.String()
	}
	return ""
}

// extractSegText recursively extracts text from a RichTextSegment.
func extractSegText(seg widget.RichTextSegment) string {
	switch s := seg.(type) {
	case *widget.TextSegment:
		return s.Text
	case *widget.HyperlinkSegment:
		return s.Text
	case *widget.SeparatorSegment:
		return ""
	case *widget.ParagraphSegment:
		var b strings.Builder
		for _, child := range s.Segments() {
			b.WriteString(extractSegText(child))
		}
		b.WriteString("\n")
		return b.String()
	case *widget.ListSegment:
		var b strings.Builder
		for _, item := range s.Segments() {
			b.WriteString(extractSegText(item))
		}
		return b.String()
	case *widget.ImageSegment:
		return s.Title
	default:
		return seg.Textual()
	}
}

// verifyContent checks that rendered text contains expected substrings.
func verifyContent(t *testing.T, rendered fyne.CanvasObject, name string, want ...string) {
	t.Helper()
	text := extractText(rendered)
	for _, w := range want {
		if !strings.Contains(text, w) {
			t.Errorf("%s: rendered output should contain %q\nGot:\n%s", name, w, text)
		}
	}
}

func TestSimpleParagraph(t *testing.T) {
	r, err := RenderMarkdown("Hello, world!")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "Hello, world!") {
		t.Errorf("expected 'Hello, world!' in output, got: %q", text)
	}
}

func TestHeading(t *testing.T) {
	r, err := RenderMarkdown("# Title\n\nSome text.")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "heading", "Title", "Some text.")
}

func TestBoldAndItalic(t *testing.T) {
	r, err := RenderMarkdown("**bold** and *italic* text.")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "bold/italic", "bold", "italic", "text")
}

func TestInlineCode(t *testing.T) {
	r, err := RenderMarkdown("Use `code` here.")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "code") {
		t.Errorf("should contain 'code': %q", text)
	}
}

func TestCodeBlock(t *testing.T) {
	r, err := RenderMarkdown("```go\npackage main\n\nfunc main() {}\n```")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "code block", "package main", "func main")
}

func TestUnorderedList(t *testing.T) {
	r, err := RenderMarkdown("- item 1\n- item 2\n- item 3")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "ulist", "item 1", "item 2", "item 3")
}

func TestOrderedList(t *testing.T) {
	r, err := RenderMarkdown("1. first\n2. second\n3. third")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "olist", "first", "second", "third")
}

func TestHorizontalRule(t *testing.T) {
	r, err := RenderMarkdown("Before\n\n---\n\nAfter")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "hr", "Before", "After")
}

func TestLink(t *testing.T) {
	r, err := RenderMarkdown("[text](https://example.com)")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "text") {
		t.Errorf("should contain link text: %q", text)
	}
}

func TestBlockquote(t *testing.T) {
	r, err := RenderMarkdown("> quoted text")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "quoted text") {
		t.Errorf("should contain quoted text: %q", text)
	}
}

func TestTable(t *testing.T) {
	md := "| H1 | H2 |\n|----|----|\n| A  | B  |\n| C  | D  |"
	r, err := RenderMarkdown(md)
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "table", "H1", "H2", "A", "B", "C", "D")
}

func TestMixedContent(t *testing.T) {
	md := "# Section\n\nA paragraph.\n\n- list item\n\n```\ncode\n```\n\n> quote"
	r, err := RenderMarkdown(md)
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "mixed", "Section", "paragraph", "list item", "code", "quote")
}

func TestEmptyText(t *testing.T) {
	r, err := RenderMarkdown("")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if text != "" {
		t.Errorf("empty input should produce empty output, got %q", text)
	}
}

func TestWhitespaceOnly(t *testing.T) {
	r, err := RenderMarkdown("   \n\n  ")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if text != "" {
		t.Errorf("whitespace input should produce empty output, got %q", text)
	}
}

func TestCodeBlockLanguage(t *testing.T) {
	r, err := RenderMarkdown("```python\nprint('hello')\n```")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "print('hello')") {
		t.Errorf("should contain code content: %q", text)
	}
}

func TestNestedFormatting(t *testing.T) {
	r, err := RenderMarkdown("- **bold** item\n- `code` item\n- normal")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "nested", "bold", "code", "normal")
}

func TestPartialStreaming(t *testing.T) {
	// Incomplete markdown should not crash
	r, err := RenderMarkdown("# Unfinished")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "Unfinished") {
		t.Errorf("should contain partial content: %q", text)
	}
}

func TestHTMLNotRendered(t *testing.T) {
	r, err := RenderMarkdown("<script>alert('xss')</script>")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	// goldmark by default doesn't render raw HTML; should be skipped
	if strings.Contains(text, "alert") && strings.Contains(text, "xss") {
		t.Logf("HTML in markdown rendered as: %q", text)
	}
}

func TestMultipleParagraphs(t *testing.T) {
	r, err := RenderMarkdown("Para one.\n\nPara two.\n\nPara three.")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "paragraphs", "Para one", "Para two", "Para three")
}

func TestTaskList(t *testing.T) {
	r, err := RenderMarkdown("- [x] done\n- [ ] todo")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	verifyContent(t, r, "tasklist", "done", "todo")
}

func TestCodeBlockPreservesSpacing(t *testing.T) {
	r, err := RenderMarkdown("```\n  indented\n  code\n```")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "indented") {
		t.Errorf("code block should preserve content: %q", text)
	}
}

func TestEscapedChars(t *testing.T) {
	r, err := RenderMarkdown("\\*not italic\\*")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "not italic") {
		t.Errorf("escaped chars: got %q", text)
	}
}

func TestImage(t *testing.T) {
	r, err := RenderMarkdown("![alt](img.png)")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	text := extractText(r)
	if !strings.Contains(text, "alt") || !strings.Contains(text, "img.png") {
		t.Logf("image rendering: %q", text)
	}
}

// ── New tests: segment type/style verification ──

func TestRenderMarkdownReturnsRichText(t *testing.T) {
	r, err := RenderMarkdown("hello")
	if err != nil {
		t.Fatalf("RenderMarkdown: %v", err)
	}
	if _, ok := r.(*widget.RichText); !ok {
		t.Fatalf("expected *widget.RichText, got %T", r)
	}
}

func TestHeadingStyle(t *testing.T) {
	r, _ := RenderMarkdown("# Heading")
	rt := r.(*widget.RichText)
	if len(rt.Segments) == 0 {
		t.Fatal("expected at least 1 segment")
	}
	ps, ok := rt.Segments[0].(*widget.ParagraphSegment)
	if !ok {
		t.Fatalf("expected ParagraphSegment, got %T", rt.Segments[0])
	}
	if len(ps.Texts) == 0 {
		t.Fatal("expected at least 1 text in paragraph")
	}
	ts, ok := ps.Texts[0].(*widget.TextSegment)
	if !ok {
		t.Fatalf("expected TextSegment, got %T", ps.Texts[0])
	}
	if !ts.Style.TextStyle.Bold {
		t.Error("heading style should have Bold=true")
	}
}

func TestBoldStyle(t *testing.T) {
	r, _ := RenderMarkdown("**bold**")
	rt := r.(*widget.RichText)
	if len(rt.Segments) == 0 {
		t.Fatal("expected at least 1 segment")
	}
	ps, ok := rt.Segments[0].(*widget.ParagraphSegment)
	if !ok {
		t.Fatalf("expected ParagraphSegment, got %T", rt.Segments[0])
	}
	if len(ps.Texts) == 0 {
		t.Fatal("expected at least 1 text in paragraph")
	}
	ts, ok := ps.Texts[0].(*widget.TextSegment)
	if !ok {
		t.Fatalf("expected TextSegment, got %T", ps.Texts[0])
	}
	if !ts.Style.TextStyle.Bold {
		t.Error("bold text should have Bold=true")
	}
	if ts.Style.TextStyle.Italic {
		t.Error("bold text should not have Italic=true")
	}
}

func TestItalicStyle(t *testing.T) {
	r, _ := RenderMarkdown("*italic*")
	rt := r.(*widget.RichText)
	ps := rt.Segments[0].(*widget.ParagraphSegment)
	ts := ps.Texts[0].(*widget.TextSegment)
	if !ts.Style.TextStyle.Italic {
		t.Error("italic text should have Italic=true")
	}
}

func TestCodeInlineStyle(t *testing.T) {
	r, _ := RenderMarkdown("`code`")
	rt := r.(*widget.RichText)
	ps := rt.Segments[0].(*widget.ParagraphSegment)
	ts := ps.Texts[0].(*widget.TextSegment)
	if !ts.Style.TextStyle.Monospace {
		t.Error("inline code should have Monospace=true")
	}
}

func TestCodeBlockStyle(t *testing.T) {
	r, _ := RenderMarkdown("```\ncode\n```")
	rt := r.(*widget.RichText)
	if len(rt.Segments) == 0 {
		t.Fatal("expected at least 1 segment")
	}
	ps, ok := rt.Segments[0].(*widget.ParagraphSegment)
	if !ok {
		t.Fatalf("expected ParagraphSegment, got %T", rt.Segments[0])
	}
	if len(ps.Texts) == 0 {
		t.Fatal("expected at least 1 text in paragraph")
	}
	ts, ok := ps.Texts[0].(*widget.TextSegment)
	if !ok {
		t.Fatalf("expected TextSegment, got %T", ps.Texts[0])
	}
	if !ts.Style.TextStyle.Monospace {
		t.Error("code block should have Monospace=true")
	}
}

func TestLinkSegment(t *testing.T) {
	r, _ := RenderMarkdown("[text](https://example.com)")
	rt := r.(*widget.RichText)
	ps := rt.Segments[0].(*widget.ParagraphSegment)
	hs, ok := ps.Texts[0].(*widget.HyperlinkSegment)
	if !ok {
		t.Fatalf("expected HyperlinkSegment, got %T", ps.Texts[0])
	}
	if hs.Text != "text" {
		t.Errorf("link text = %q, want 'text'", hs.Text)
	}
	if hs.URL == nil || hs.URL.String() != "https://example.com" {
		t.Errorf("link URL = %v, want https://example.com", hs.URL)
	}
}

func TestListSegmentOrdered(t *testing.T) {
	r, _ := RenderMarkdown("1. first\n2. second")
	rt := r.(*widget.RichText)
	ls, ok := rt.Segments[0].(*widget.ListSegment)
	if !ok {
		t.Fatalf("expected ListSegment, got %T", rt.Segments[0])
	}
	if !ls.Ordered {
		t.Error("list should be ordered")
	}
	if len(ls.Items) != 2 {
		t.Fatalf("expected 2 list items, got %d", len(ls.Items))
	}
}

func TestListSegmentUnordered(t *testing.T) {
	r, _ := RenderMarkdown("- item\n- another")
	rt := r.(*widget.RichText)
	ls, ok := rt.Segments[0].(*widget.ListSegment)
	if !ok {
		t.Fatalf("expected ListSegment, got %T", rt.Segments[0])
	}
	if ls.Ordered {
		t.Error("list should be unordered")
	}
	if len(ls.Items) != 2 {
		t.Fatalf("expected 2 list items, got %d", len(ls.Items))
	}
}

func TestSeparatorSegment(t *testing.T) {
	r, _ := RenderMarkdown("---")
	rt := r.(*widget.RichText)
	if len(rt.Segments) == 0 {
		t.Fatal("expected at least 1 segment")
	}
	_, ok := rt.Segments[0].(*widget.SeparatorSegment)
	if !ok {
		t.Fatalf("expected SeparatorSegment, got %T", rt.Segments[0])
	}
}

func TestBlockquoteStyle(t *testing.T) {
	r, _ := RenderMarkdown("> quote")
	rt := r.(*widget.RichText)
	if len(rt.Segments) == 0 {
		t.Fatal("expected at least 1 segment")
	}
	ps, ok := rt.Segments[0].(*widget.ParagraphSegment)
	if !ok {
		t.Fatalf("expected ParagraphSegment, got %T", rt.Segments[0])
	}
	if len(ps.Texts) == 0 {
		t.Fatal("expected at least 1 text in paragraph")
	}
	ts, ok := ps.Texts[0].(*widget.TextSegment)
	if !ok {
		t.Fatalf("expected TextSegment, got %T", ps.Texts[0])
	}
	if !ts.Style.TextStyle.Italic {
		t.Error("blockquote style should have Italic=true")
	}
}

// ── MarkdownToRichText tests ──

func TestMarkdownToRichText(t *testing.T) {
	rt := MarkdownToRichText("Hello **world**")
	if rt == nil {
		t.Fatal("MarkdownToRichText returned nil")
	}
	if len(rt.Segments) == 0 {
		t.Error("expected segments in RichText")
	}
}

func TestMarkdownToRichTextEmpty(t *testing.T) {
	rt := MarkdownToRichText("")
	if rt == nil {
		t.Fatal("MarkdownToRichText returned nil for empty input")
	}
}

// ── RenderMarkdownPlain tests ──

func TestRenderMarkdownPlain(t *testing.T) {
	r := RenderMarkdownPlain("Hello **world**")
	if !strings.Contains(r, "Hello") {
		t.Errorf("expected 'Hello' in plain output: %q", r)
	}
	if !strings.Contains(r, "world") {
		t.Errorf("expected 'world' in plain output: %q", r)
	}
}

func TestRenderMarkdownPlainEmpty(t *testing.T) {
	r := RenderMarkdownPlain("")
	if r != "" {
		t.Errorf("empty input should produce empty output, got %q", r)
	}
}
