package renderer

import (
	"bytes"
	"fmt"
	"net/url"
	"strings"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/widget"
	"github.com/yuin/goldmark"
	gast "github.com/yuin/goldmark/ast"
	"github.com/yuin/goldmark/extension"
	extensionAST "github.com/yuin/goldmark/extension/ast"
	"github.com/yuin/goldmark/text"
)

// RenderMarkdown parses markdown text and returns a Fyne CanvasObject
// suitable for display in a widget.RichText or container.
func RenderMarkdown(md string) (fyne.CanvasObject, error) {
	if strings.TrimSpace(md) == "" {
		return widget.NewRichText(), nil
	}

	source := []byte(md)
	reader := text.NewReader(source)

	mdParser := goldmark.New(
		goldmark.WithExtensions(extension.GFM),
	)
	doc := mdParser.Parser().Parse(reader)

	r := &mdRenderer{source: source}
	r.render(doc)

	segments := r.segs
	if segments == nil {
		segments = []widget.RichTextSegment{}
	}
	return widget.NewRichText(segments...), nil
}

// MarkdownToRichText returns a *widget.RichText populated with segments.
func MarkdownToRichText(text string) *widget.RichText {
	obj, err := RenderMarkdown(text)
	if err != nil {
		return widget.NewRichText()
	}
	if rt, ok := obj.(*widget.RichText); ok {
		return rt
	}
	return widget.NewRichText()
}

// RenderMarkdownPlain converts Markdown text to a clean readable plain-text representation.
// This is a fallback for contexts where Fyne widgets are not available.
func RenderMarkdownPlain(md string) string {
	if strings.TrimSpace(md) == "" {
		return ""
	}

	source := []byte(md)
	reader := text.NewReader(source)

	mdParser := goldmark.New(
		goldmark.WithExtensions(extension.GFM),
	)
	doc := mdParser.Parser().Parse(reader)

	var buf bytes.Buffer
	renderPlainNode(doc, &buf, source, 0)
	return strings.TrimSpace(buf.String())
}

// ── Fyne RichText segment builder ──

type mdRenderer struct {
	source []byte
	segs   []widget.RichTextSegment
}

func (r *mdRenderer) render(node gast.Node) {
	if node == nil {
		return
	}

	switch n := node.(type) {
	case *gast.Document:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.render(child)
		}

	case *gast.Paragraph:
		inlineSegs := r.renderInline(n)
		r.segs = append(r.segs, &widget.ParagraphSegment{Texts: inlineSegs})

	case *gast.Heading:
		inlineSegs := r.renderInline(n)
		style := widget.RichTextStyleHeading
		if n.Level > 1 {
			style = widget.RichTextStyleSubHeading
		}
		for _, s := range inlineSegs {
			if ts, ok := s.(*widget.TextSegment); ok {
				ts.Style = style
			}
		}
		r.segs = append(r.segs, &widget.ParagraphSegment{Texts: inlineSegs})

	case *gast.TextBlock:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.render(child)
		}

	case *gast.FencedCodeBlock:
		var codeBuf bytes.Buffer
		lines := n.Lines()
		for i := 0; i < lines.Len(); i++ {
			seg := lines.At(i)
			codeBuf.Write(seg.Value(r.source))
		}
		codeText := codeBuf.String()
		r.segs = append(r.segs, &widget.ParagraphSegment{
			Texts: []widget.RichTextSegment{
				&widget.TextSegment{
					Style: widget.RichTextStyleCodeBlock,
					Text:  codeText,
				},
			},
		})

	case *gast.CodeBlock:
		var codeBuf bytes.Buffer
		lines := n.Lines()
		for i := 0; i < lines.Len(); i++ {
			seg := lines.At(i)
			codeBuf.Write(seg.Value(r.source))
		}
		codeText := codeBuf.String()
		r.segs = append(r.segs, &widget.ParagraphSegment{
			Texts: []widget.RichTextSegment{
				&widget.TextSegment{
					Style: widget.RichTextStyleCodeBlock,
					Text:  codeText,
				},
			},
		})

	case *gast.List:
		listSeg := &widget.ListSegment{
			Ordered: n.IsOrdered(),
			Items:   []widget.RichTextSegment{},
		}
		start := n.Start
		if start > 1 {
			listSeg.SetStartNumber(start)
		}
		// Collect item segments
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			itemSegs := r.renderInline(child)
			// Wrap item content in a ParagraphSegment for each item
			itemPara := &widget.ParagraphSegment{Texts: itemSegs}
			listSeg.Items = append(listSeg.Items, itemPara)
		}
		if len(listSeg.Items) > 0 {
			r.segs = append(r.segs, listSeg)
		}

	case *gast.ListItem:
		// ListItem children are rendered inline (handled by parent List case above)
		// This case is for when ListItem is rendered standalone
		inlineSegs := r.renderInline(n)
		for _, s := range inlineSegs {
			r.segs = append(r.segs, s)
		}

	case *gast.ThematicBreak:
		r.segs = append(r.segs, &widget.SeparatorSegment{})

	case *extensionAST.Table:
		// Render table cells as pipe-separated monospace text
		var tableText bytes.Buffer
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.renderTableRow(child, &tableText)
		}
		if tableText.Len() > 0 {
			r.segs = append(r.segs, &widget.ParagraphSegment{
				Texts: []widget.RichTextSegment{
					&widget.TextSegment{
						Style: widget.RichTextStyleCodeBlock,
						Text:  tableText.String(),
					},
				},
			})
		}

	case *gast.Blockquote:
		var quoteSegs []widget.RichTextSegment
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			quoteSegs = append(quoteSegs, r.renderInline(child)...)
		}
		for _, s := range quoteSegs {
			if ts, ok := s.(*widget.TextSegment); ok {
				ts.Style = widget.RichTextStyleBlockquote
			}
		}
		if len(quoteSegs) > 0 {
			r.segs = append(r.segs, &widget.ParagraphSegment{Texts: quoteSegs})
		}

	default:
		// Fallback: render children
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.render(child)
		}
	}
}

// renderInline handles inline content and returns a slice of segments.
// renderTableRow collects cell text from a table row (or header) into a pipe-separated line.
func (r *mdRenderer) renderTableRow(node gast.Node, buf *bytes.Buffer) {
	switch n := node.(type) {
	case *extensionAST.TableHeader:
		buf.WriteString("| ")
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.renderTableCell(child, buf)
			buf.WriteString(" | ")
		}
		buf.WriteByte('\n')
	case *extensionAST.TableRow:
		buf.WriteString("| ")
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			r.renderTableCell(child, buf)
			buf.WriteString(" | ")
		}
		buf.WriteByte('\n')
	}
}

// renderTableCell collects text from a table cell.
func (r *mdRenderer) renderTableCell(node gast.Node, buf *bytes.Buffer) {
	switch n := node.(type) {
	case *extensionAST.TableCell:
		inlineSegs := r.renderInline(n)
		for _, s := range inlineSegs {
			if ts, ok := s.(*widget.TextSegment); ok {
				buf.WriteString(ts.Text)
			}
		}
	}
}

func (r *mdRenderer) renderInline(node gast.Node) []widget.RichTextSegment {
	var segs []widget.RichTextSegment

	var walk func(n gast.Node)
	walk = func(n gast.Node) {
		if n == nil {
			return
		}
		switch v := n.(type) {
		case *gast.Text:
			val := string(v.Segment.Value(r.source))
			if v.SoftLineBreak() {
				val += "\n"
			}
			segs = append(segs, &widget.TextSegment{
				Style: widget.RichTextStyleInline,
				Text:  val,
			})

		case *gast.String:
			segs = append(segs, &widget.TextSegment{
				Style: widget.RichTextStyleInline,
				Text:  string(v.Value),
			})

		case *gast.CodeSpan:
			var codeBuf bytes.Buffer
			for child := v.FirstChild(); child != nil; child = child.NextSibling() {
				if t, ok := child.(*gast.Text); ok {
					codeBuf.Write(t.Segment.Value(r.source))
				}
			}
			segs = append(segs, &widget.TextSegment{
				Style: widget.RichTextStyleCodeInline,
				Text:  codeBuf.String(),
			})

		case *gast.Emphasis:
			style := widget.RichTextStyleEmphasis
			if v.Level == 2 {
				style = widget.RichTextStyleStrong
			}
			for child := v.FirstChild(); child != nil; child = child.NextSibling() {
				childSegs := r.renderInline(child)
				for _, s := range childSegs {
					if ts, ok := s.(*widget.TextSegment); ok {
						ts.Style = style
					}
					segs = append(segs, s)
				}
			}

		case *gast.Link:
			dest := string(v.Destination)
			parsedURL, err := url.Parse(dest)
			linkText := extractInlineText(v, r.source)
			hs := &widget.HyperlinkSegment{
				Text: linkText,
			}
			if err == nil && parsedURL != nil {
				hs.URL = parsedURL
			}
			segs = append(segs, hs)

		case *gast.Image:
			alt := extractInlineText(v, r.source)
			dest := string(v.Destination)
			segs = append(segs, &widget.TextSegment{
				Style: widget.RichTextStyleInline,
				Text:  fmt.Sprintf("[%s](%s)", alt, dest),
			})

		case *gast.AutoLink:
			urlStr := string(v.URL(r.source))
			parsedURL, _ := url.Parse(urlStr)
			hs := &widget.HyperlinkSegment{
				Text: urlStr,
			}
			if parsedURL != nil {
				hs.URL = parsedURL
			}
			segs = append(segs, hs)

		case *extensionAST.Strikethrough:
			text := collectText(v, r.source)
			segs = append(segs, &widget.TextSegment{
				Style: widget.RichTextStyleInline,
				Text:  "~~" + text + "~~",
			})

		case *extensionAST.TaskCheckBox:
			if v.IsChecked {
				segs = append(segs, &widget.TextSegment{
					Style: widget.RichTextStyleInline,
					Text:  "[x] ",
				})
			} else {
				segs = append(segs, &widget.TextSegment{
					Style: widget.RichTextStyleInline,
					Text:  "[ ] ",
				})
			}

		case *gast.RawHTML:
			// Skip raw HTML

		default:
			// Walk children for unknown inline nodes
			for child := n.FirstChild(); child != nil; child = child.NextSibling() {
				walk(child)
			}
		}
	}

	walk(node)
	return segs
}

// extractInlineText collects the text content from an inline node.
func extractInlineText(node gast.Node, source []byte) string {
	var buf bytes.Buffer
	for child := node.FirstChild(); child != nil; child = child.NextSibling() {
		if t, ok := child.(*gast.Text); ok {
			buf.Write(t.Segment.Value(source))
		} else {
			buf.WriteString(extractInlineText(child, source))
		}
	}
	return buf.String()
}

// collectText recursively collects text from all descendant text nodes.
func collectText(node gast.Node, source []byte) string {
	var buf bytes.Buffer
	for child := node.FirstChild(); child != nil; child = child.NextSibling() {
		switch v := child.(type) {
		case *gast.Text:
			buf.Write(v.Segment.Value(source))
		default:
			buf.WriteString(collectText(child, source))
		}
	}
	return buf.String()
}

// ── Plain text renderer (original implementation) ──

func renderPlainNode(node gast.Node, buf *bytes.Buffer, source []byte, depth int) {
	if node == nil {
		return
	}

	indent := strings.Repeat("  ", depth)

	switch n := node.(type) {
	case *gast.Document:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
		}

	case *gast.Paragraph:
		renderPlainInlineContent(n, buf, source)
		buf.WriteString("\n\n")

	case *gast.Heading:
		renderPlainInlineContent(n, buf, source)
		buf.WriteString("\n\n")

	case *gast.TextBlock:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
		}

	case *gast.Text:
		val := string(n.Segment.Value(source))
		if n.SoftLineBreak() {
			buf.WriteByte('\n')
		}
		buf.WriteString(val)

	case *gast.String:
		buf.Write(n.Value)

	case *gast.CodeSpan:
		buf.WriteByte('`')
		renderPlainInlineContent(n, buf, source)
		buf.WriteByte('`')

	case *gast.FencedCodeBlock:
		buf.WriteByte('\n')
		lines := n.Lines()
		for i := 0; i < lines.Len(); i++ {
			seg := lines.At(i)
			buf.Write(seg.Value(source))
		}

	case *gast.CodeBlock:
		buf.WriteByte('\n')
		lines := n.Lines()
		for i := 0; i < lines.Len(); i++ {
			seg := lines.At(i)
			buf.Write(seg.Value(source))
		}

	case *gast.List:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
		}

	case *gast.ListItem:
		prefix := "- "
		parent := n.Parent()
		if parent != nil {
			if lst, ok := parent.(*gast.List); ok && lst.IsOrdered() {
				idx := 0
				for c := lst.FirstChild(); c != nil && c != n; c = c.NextSibling() {
					idx++
				}
				prefix = string(rune('1'+idx)) + ". "
			}
		}
		// Check for task list checkbox
		if tc := getTaskCheckBox(n); tc != nil {
			if tc.IsChecked {
				prefix = "- [x] "
			} else {
				prefix = "- [ ] "
			}
		}
		buf.WriteString(indent + prefix)
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			if tb, ok := child.(*gast.TextBlock); ok {
				renderPlainInlineContent(tb, buf, source)
			} else if _, ok := child.(*extensionAST.TaskCheckBox); ok {
				// Skip — already handled in prefix
			} else {
				renderPlainNode(child, buf, source, depth+1)
			}
		}
		buf.WriteByte('\n')

	case *gast.ThematicBreak:
		buf.WriteString("---\n\n")

	case *gast.Link:
		renderPlainInlineContent(n, buf, source)
		dest := string(n.Destination)
		if dest != "" {
			buf.WriteString(" <" + dest + ">")
		}

	case *gast.Image:
		alt := altTextPlain(n, source)
		dest := string(n.Destination)
		buf.WriteString("[" + alt + "](" + dest + ")")

	case *gast.Blockquote:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			buf.WriteString("> ")
			renderPlainNode(child, buf, source, depth)
		}
		buf.WriteByte('\n')

	case *extensionAST.Table:
		buf.WriteByte('\n')
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth+1)
		}
		buf.WriteByte('\n')

	case *extensionAST.TableHeader:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
		}

	case *extensionAST.TableRow:
		buf.WriteString(indent + "| ")
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
			buf.WriteString(" | ")
		}
		buf.WriteByte('\n')

	case *extensionAST.TableCell:
		renderPlainNode(n.FirstChild(), buf, source, depth)

	case *gast.Emphasis:
		renderPlainInlineContent(n, buf, source)

	case *extensionAST.Strikethrough:
		buf.WriteString("~~")
		renderPlainInlineContent(n, buf, source)
		buf.WriteString("~~")

	case *gast.AutoLink:
		buf.WriteString(string(n.URL(source)))

	case *gast.RawHTML:
		// Skip raw HTML

	case *gast.HTMLBlock:
		// Skip raw HTML blocks

	default:
		for child := n.FirstChild(); child != nil; child = child.NextSibling() {
			renderPlainNode(child, buf, source, depth)
		}
	}
}

func renderPlainInlineContent(node gast.Node, buf *bytes.Buffer, source []byte) {
	for child := node.FirstChild(); child != nil; child = child.NextSibling() {
		renderPlainNode(child, buf, source, 0)
	}
}

func altTextPlain(image *gast.Image, source []byte) string {
	var buf bytes.Buffer
	for child := image.FirstChild(); child != nil; child = child.NextSibling() {
		if text, ok := child.(*gast.Text); ok {
			buf.Write(text.Segment.Value(source))
		}
	}
	return buf.String()
}

// getTaskCheckBox finds a TaskCheckBox child node.
func getTaskCheckBox(n gast.Node) *extensionAST.TaskCheckBox {
	for child := n.FirstChild(); child != nil; child = child.NextSibling() {
		if tc, ok := child.(*extensionAST.TaskCheckBox); ok {
			return tc
		}
	}
	return nil
}
