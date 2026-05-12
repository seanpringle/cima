# Phase 6: Markdown Renderer

## Goal

Implement a renderer that converts Markdown text into Fyne `CanvasObject` tree (or a `widget.RichText` with segments) so that assistant messages are displayed with formatting (headers, code blocks, lists, tables, inline formatting).

## Files

| File | Purpose |
|------|---------|
| `renderer/markdown.go` | `MarkdownRenderer` — converts markdown to Fyne widgets |
| `renderer/markdown_test.go` | Tests for markdown parsing/rendering |

---

## Step 6.1: renderer/markdown.go

### Type

```go
package renderer

// RenderMarkdown parses markdown text and returns a Fyne CanvasObject
// suitable for display in a widget.RichText or container containing Labels/separators.
func RenderMarkdown(text string) (fyne.CanvasObject, error)

// MarkdownToRichText returns a *widget.RichText populated with segments.
func MarkdownToRichText(text string) *widget.RichText

// RenderMarkdownPlain converts Markdown text to a clean readable plain-text
// representation. This is a fallback for contexts where Fyne widgets are not
// available (e.g. CLI or testing).
func RenderMarkdownPlain(md string) string
```

### Approach

Use **goldmark** (https://github.com/yuin/goldmark) to parse Markdown into an AST, then walk the AST and produce Fyne widgets:

| Markdown Element | Fyne Widget/Segment |
|------------------|---------------------|
| Paragraph text | `widget.RichText` with `TextSegment` |
| Heading (H1-H6) | `TextSegment` with larger/bold `TextStyle{Size: ...}` |
| Bold text | `TextSegment` with `TextStyle{Bold: true}` |
| Italic text | `TextSegment` with `TextStyle{Italic: true}` |
| Code inline | `TextSegment` with `TextStyle{Monospace: true}` |
| Code block | `widget.Label` with monospace font + background color (RichText doesn't support background; use a container with a colored rectangle) |
| Unordered list | Indented `TextSegment`s with bullet prefix "• " |
| Ordered list | Indented `TextSegment`s with "1. " prefix |
| Task list | Checkbox + text |
| Horizontal rule | `widget.Separator` |
| Link | `HyperlinkSegment` |
| Image | `widget.Icon` or placeholder text |
| Table | `widget.Table` with `GridWrapLayout` or nested `BorderLayout` cells |
| Blockquote | Indented, italicized, gray text |

### Fallback (MVP)

For the initial implementation, render markdown as plain text with basic formatting:

```go
// SimpleMarkdownToLabels parses markdown and returns a list of Fyne CanvasObjects.
// This is a simplified version that handles:
// - Headers (bold text with "### " prefix stripped)
// - Code blocks (monospace text in a bordered background)
// - Lists (bullet/ordered with indentation)
// - Inline code (monospace)
// - Paragraphs (wrapped text)
func SimpleMarkdownToLabels(text string) []fyne.CanvasObject
```

### Behaviour

- Handle streaming partial Markdown gracefully (goldmark may fail on incomplete input — fall back to plain text)
- Code blocks should have a subtle gray background
- Auto-scroll to latest content (handled by the chat tab UI, not the renderer)
- Whitespace collapse for inline text (not code blocks)
- Tables rendered with grid layout (minimum viable: pipe-separated text in monospace as fallback)

### Failing Tests: `renderer/markdown_test.go`

1. **TestSimpleParagraph** — plain text → single paragraph output
2. **TestHeading** — `# Title` → bold/larger text
3. **TestBoldAndItalic** — `**bold**` and `*italic*`
4. **TestInlineCode** — `` `code` `` → monospace
5. **TestCodeBlock** — triple-backtick fenced code → monospace block with background
6. **TestUnorderedList** — `- item` → bullet list
7. **TestOrderedList** — `1. item` → numbered list
8. **TestTaskList** — `- [x] done` and `- [ ] todo` → checkbox + text
9. **TestHorizontalRule** — `---` → separator
10. **TestLink** — `[text](url)` → hyperlink
11. **TestTable** — simple markdown table
12. **TestBlockquote** — `> quote` → indented gray text
13. **TestMixedContent** — paragraph, heading, code block, list all together
14. **TestEmptyText** — empty string → nothing
15. **TestWhitespaceOnly** — whitespace → nothing
16. **TestCodeBlockLanguage** — fenced code with language identifier (rendered same as without)
17. **TestNestedFormatting** — bold inside list item, code inside paragraph
18. **TestPartialStreaming** — incomplete markdown during streaming (graceful degradation)
19. **TestHTMLNotRendered** — embedded HTML tags are escaped
20. **TestXSSAttempt** — `<script>` tags are safely escaped

---

## Running Phase 6 Tests

```bash
cd go
go test ./renderer/...
```
