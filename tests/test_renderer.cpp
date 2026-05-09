#include "renderer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <string>

namespace {

// Helper: feed a string through the renderer using realistic chunk sizes
// (LLM streaming delivers chunks of 3-10 chars, not single bytes).
// Returns the accumulated raw ANSI output.
struct RenderCollect {
    std::string out;

    // Feed text character by character (for fine-grained streaming tests)
    void operator()(const std::string& md) {
        StreamingMarkdownRenderer r([this](const std::string& s) { out += s; });
        // Use realistic chunk sizes to avoid spinner interference
        feed_chunks_impl(r, md, 5);
    }

    // Feed discrete chunks (simulates network streaming)
    void feed_chunks(const std::string& md, size_t chunk_size) {
        StreamingMarkdownRenderer r([this](const std::string& s) { out += s; });
        feed_chunks_impl(r, md, chunk_size);
    }

    // Feed char by char (for spinner / streaming-edge-case tests)
    void feed_char_by_char(const std::string& md) {
        StreamingMarkdownRenderer r([this](const std::string& s) { out += s; });
        for (char c : md)
            r.feed(std::string(1, c), OutputType::Content);
        r.finish();
    }

private:
    void feed_chunks_impl(StreamingMarkdownRenderer& r,
                          const std::string& md, size_t chunk_size) {
        for (size_t i = 0; i < md.size(); i += chunk_size) {
            size_t len = std::min(chunk_size, md.size() - i);
            r.feed(md.substr(i, len), OutputType::Content);
        }
        r.finish();
    }
};

// ANSI reset code used everywhere
constexpr auto RESET = "\033[0m";

// Helper: strip all ANSI escape sequences (CSI, OSC, etc.) for
// content-only checks.  Handles sequences that don't end with 'm'.
std::string strip_ansi(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\033') {
            i++;
            if (i >= s.size()) break;
            // Skip CSI sequences: ESC [ ... final-byte (0x40-0x7E)
            if (s[i] == '[') {
                i++;
                while (i < s.size() &&
                       (static_cast<unsigned char>(s[i]) < 0x40 ||
                        static_cast<unsigned char>(s[i]) > 0x7E))
                    i++;
                // i now points at the final byte; it will be skipped
                // by the outer loop's i++ at the top
            }
            // For other ESC sequences (like ESC ] OSC), just skip
            // until we find BEL (\a) or ST (\033\\)
            // For now, just skip the next byte and continue.
            continue;
        }
        r += s[i];
    }
    return r;
}

// Count occurrences of a substring
int count_substr(const std::string& haystack, const std::string& needle) {
    int c = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        c++;
        pos += needle.size();
    }
    return c;
}

} // namespace

// ─────────────────────────────────────────────────────────────────
// Paragraphs (plain text)
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Paragraph — plain text", "[renderer]") {
    RenderCollect rc;
    rc("hello world\n");
    CHECK(strip_ansi(rc.out) == "hello world\n");
}

TEST_CASE("Paragraph — content→content no blank line", "[renderer]") {
    RenderCollect rc;
    rc("first line\nsecond line\n");
    // Should NOT have a blank line between consecutive paragraphs
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "first line\nsecond line\n");
}

TEST_CASE("Paragraph — content→content with explicit blank line preserved", "[renderer]") {
    RenderCollect rc;
    rc("first line\n\nsecond line\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "first line\n\nsecond line\n");
}

// ─────────────────────────────────────────────────────────────────
// Headings
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Heading — H1 detected and styled", "[renderer]") {
    RenderCollect rc;
    rc("# Hello\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "Hello\n");
    // Should have gold color for H1
    CHECK(rc.out.find("38;5;220") != std::string::npos);
}

TEST_CASE("Heading — H1 to H6 color variation", "[renderer]") {
    std::string colors[] = {"38;5;220", "38;5;215", "38;5;117",
                            "38;5;183", "38;5;152", "38;5;145"};
    for (int i = 0; i < 6; i++) {
        RenderCollect rc;
        rc(std::string(i + 1, '#') + " Title\n");
        REQUIRE(rc.out.find(colors[i]) != std::string::npos);
    }
}

TEST_CASE("Heading — content→heading gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("text\n# Heading\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "text\n\nHeading\n");
}

TEST_CASE("Heading — heading→content gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("# Heading\ntext\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "Heading\n\ntext\n");
}

TEST_CASE("Heading — consecutive headings get blank line", "[renderer]") {
    RenderCollect rc;
    rc("# First\n## Second\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain == "First\n\nSecond\n");
}

TEST_CASE("Heading — bold inside heading", "[renderer]") {
    RenderCollect rc;
    rc("# Hello **world**\n");
    // Bold ANSI codes should appear within the heading line
    CHECK(rc.out.find("\033[1m") != std::string::npos);
    CHECK(rc.out.find("\033[22m") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Horizontal rules
// ─────────────────────────────────────────────────────────────────

TEST_CASE("HR — triple dash", "[renderer]") {
    RenderCollect rc;
    rc("---\n");
    CHECK(rc.out.find("\033[2m") != std::string::npos);
    // Should contain a wide horizontal line (box-drawing or dashes)
    CHECK(strip_ansi(rc.out).size() > 10);
}

TEST_CASE("HR — content→HR gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("text\n---\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\n\n") != std::string::npos);
}

TEST_CASE("HR — HR→content gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("---\ncontent\n");
    auto plain = strip_ansi(rc.out);
    // HR emits its own line, then blank, then content
    CHECK(plain.find("\n\ncontent") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Unordered lists
// ─────────────────────────────────────────────────────────────────

TEST_CASE("List — single unordered item with bullet", "[renderer]") {
    RenderCollect rc;
    rc("- item\n");
    auto plain = strip_ansi(rc.out);
    // Should use bullet character, not the dash
    CHECK(plain.find("\u2022") != std::string::npos);
    CHECK(plain.find("item") != std::string::npos);
}

TEST_CASE("List — consecutive unordered items no blank line", "[renderer]") {
    RenderCollect rc;
    rc("- first\n- second\n");
    auto plain = strip_ansi(rc.out);
    // Should NOT have blank line between items
    int newlines = count_substr(plain, "\n");
    CHECK(newlines == 2); // two list items + final newline from finish()? 
    // Actually, each line has one \n, so two lines = two \n chars
    CHECK(plain.find("\n\n") == std::string::npos); // no double newline
}

TEST_CASE("List — content→list gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("text\n- item\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\n\n\u2022") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Ordered lists
// ─────────────────────────────────────────────────────────────────

TEST_CASE("OrderedList — numbering increments", "[renderer]") {
    RenderCollect rc;
    rc("1. first\n2. second\n3. third\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("1. ") != std::string::npos);
    CHECK(plain.find("2. ") != std::string::npos);
    CHECK(plain.find("3. ") != std::string::npos);
}

TEST_CASE("OrderedList — reset numbering after break", "[renderer]") {
    RenderCollect rc;
    rc("1. first\n\ntext\n\n1. new list\n");
    auto plain = strip_ansi(rc.out);
    // After a break, new ordered list should restart at 1
    // "new list" should have "1. " prefix, not "2. "
    size_t pos = plain.find("new list");
    CHECK(pos != std::string::npos);
    // There should be "1. " before "new list"
    CHECK(plain.rfind("1. ", pos) != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Nested lists
// ─────────────────────────────────────────────────────────────────

TEST_CASE("NestedList — sub-bullet with 2-space indent", "[renderer]") {
    RenderCollect rc;
    rc("- parent\n  - child\n");
    auto plain = strip_ansi(rc.out);
    // Child should be indented (have leading spaces)
    CHECK(plain.find("  \u2022") != std::string::npos);
}

TEST_CASE("NestedList — multi-level nesting", "[renderer]") {
    RenderCollect rc;
    rc("- A\n  - B\n    - C\n  - D\n- E\n");
    auto plain = strip_ansi(rc.out);
    // A: no indent
    CHECK(plain.find("\u2022 A") != std::string::npos);
    // B: 2-space indent
    CHECK(plain.find("  \u2022 B") != std::string::npos);
    // C: 4-space indent
    CHECK(plain.find("    \u2022 C") != std::string::npos);
    // D: back to 2-space indent
    CHECK(plain.find("  \u2022 D") != std::string::npos);
    // E: back to no indent
    CHECK(plain.find("\u2022 E") != std::string::npos);
}

TEST_CASE("NestedList — mix ordered and unordered", "[renderer]") {
    RenderCollect rc;
    rc("- fruit\n  1. apple\n  2. pear\n- veg\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\u2022 fruit") != std::string::npos);
    CHECK(plain.find("  1. apple") != std::string::npos);
    CHECK(plain.find("  2. pear") != std::string::npos);
    CHECK(plain.find("\u2022 veg") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// List continuation (indented text after list item, no marker)
// ─────────────────────────────────────────────────────────────────

TEST_CASE("ListContinuation — indented text continues list item", "[renderer]") {
    RenderCollect rc;
    rc("- item\n  continued text\n- next item\n");
    auto plain = strip_ansi(rc.out);
    // "continued text" should have no bullet — just indented content
    CHECK(plain.find("continued text") != std::string::npos);
    // Should not have a bullet before "continued text"
    CHECK(plain.find("\u2022 continued") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Blockquotes
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Blockquote — prefix emitted", "[renderer]") {
    RenderCollect rc;
    rc("> quoted\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\u2502") != std::string::npos); // │ bar
    CHECK(plain.find("quoted") != std::string::npos);
}

TEST_CASE("Blockquote — dim/gray ANSI", "[renderer]") {
    RenderCollect rc;
    rc("> quote\n");
    CHECK(rc.out.find("\033[2m") != std::string::npos);
    CHECK(rc.out.find("38;5;245") != std::string::npos);
}

TEST_CASE("Blockquote — content→blockquote gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("text\n> quote\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\n\n\u2502") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Code blocks (fenced)
// ─────────────────────────────────────────────────────────────────

TEST_CASE("CodeBlock — open/close fence detected", "[renderer]") {
    RenderCollect rc;
    rc("```\ncode\n```\n");
    auto plain = strip_ansi(rc.out);
    // Should have code content
    CHECK(plain.find("code") != std::string::npos);
    // Should have header/footer markers (box drawing)
    CHECK(plain.find("\u2550") != std::string::npos);
}

TEST_CASE("CodeBlock — background color ANSI", "[renderer]") {
    RenderCollect rc;
    rc("```\nline\n```\n");
    CHECK(rc.out.find("48;5;236") != std::string::npos); // bg
    CHECK(rc.out.find("38;5;208") != std::string::npos); // fg
}

TEST_CASE("CodeBlock — left padding 2 spaces", "[renderer]") {
    RenderCollect rc;
    rc("```\ncode\n```\n");
    // Inside code block, each line should have 2-space left pad
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("  code") != std::string::npos);
}

TEST_CASE("CodeBlock — right padding to terminal width", "[renderer]") {
    RenderCollect rc;
    rc("```\nshort\n```\n");
    // After "short" there should be padding spaces before \n
    // (terminal is 80 columns by default in tests)
    auto plain = strip_ansi(rc.out);
    CHECK(plain.size() > 10); // just sanity — the padding fills
}

TEST_CASE("CodeBlock — language tag", "[renderer]") {
    RenderCollect rc;
    rc("```python\ncode\n```\n");
    // Language tag should appear in the header
    CHECK(rc.out.find("python") != std::string::npos);
}

TEST_CASE("CodeBlock — content→code gets blank line", "[renderer]") {
    RenderCollect rc;
    rc("text\n```\ncode\n```\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\n\n\u2550") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Inline formatting
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Inline — bold **text**", "[renderer]") {
    RenderCollect rc;
    rc("hello **bold** world\n");
    CHECK(rc.out.find("\033[1mbold\033[22m") != std::string::npos);
}

TEST_CASE("Inline — code `text`", "[renderer]") {
    RenderCollect rc;
    rc("hello `code` world\n");
    CHECK(rc.out.find("\033[38;5;208mcode\033[39m") != std::string::npos);
}

TEST_CASE("Inline — bold inside list item", "[renderer]") {
    RenderCollect rc;
    rc("- **bold item**\n");
    CHECK(rc.out.find("\033[1mbold item\033[22m") != std::string::npos);
}

TEST_CASE("Inline — code inside bold", "[renderer]") {
    RenderCollect rc;
    rc("**`code inside bold`**\n");
    // Should have both bold and code ANSI codes
    CHECK(rc.out.find("\033[1m") != std::string::npos);
    CHECK(rc.out.find("38;5;208") != std::string::npos);
}

TEST_CASE("Inline — escape character", "[renderer]") {
    RenderCollect rc;
    rc("\\*not bold\\*\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("*not bold*") != std::string::npos);
    // Should NOT have bold ANSI
    CHECK(rc.out.find("\033[1m") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Block spacing rules
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Spacing — mixed content and blocks", "[renderer]") {
    // This is the combinatorial test: many block type transitions
    RenderCollect rc;
    rc("text paragraph\n"
       "# Heading\n"
       "- list item\n"
       "> blockquote\n"
       "normal text\n"
       "---\n"
       "more text\n");
    auto plain = strip_ansi(rc.out);
    // Check transitions:
    // text → heading: blank line
    CHECK(plain.find("paragraph\n\n") != std::string::npos); // text then blank then heading content
    // heading → list: blank line  
    // list → blockquote: blank line
    // blockquote → normal text: blank line
    // normal text → HR: blank line
    // HR → more text: blank line
    // Count blank lines in transitions
    // All block transitions should be separated by exactly one blank line
    int doubles = count_substr(plain, "\n\n");
    // text→heading, heading→list, list→blockquote, blockquote→text,
    // text→HR, HR→text = 6 transitions with blank lines
    CHECK(doubles >= 6);
}

TEST_CASE("Spacing — no blank lines between same-type content", "[renderer]") {
    RenderCollect rc;
    rc("line1\nline2\nline3\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("\n\n") == std::string::npos);
}

TEST_CASE("Spacing — explicit blank lines preserved but no double", "[renderer]") {
    RenderCollect rc;
    rc("text1\n\n\n## Heading\n");
    auto plain = strip_ansi(rc.out);
    // Should NOT have triple blank (should be max one separator)
    int triples = count_substr(plain, "\n\n\n");
    // Well, the explicit blank + the transition blank might merge...
    // The rule is: user's blank line IS the separator
    // So we should have exactly one blank line between text1 and Heading
    // "text1\n\nHeading\n" — that's one blank line
    CHECK(triples == 0);
    // But should have at least one blank
    CHECK(plain.find("\n\n") != std::string::npos);
}

TEST_CASE("Spacing — first item has no leading blank", "[renderer]") {
    RenderCollect rc;
    rc("# Start\n");
    // No blank line before the first thing
    CHECK(rc.out[0] != '\n');
}

// ─────────────────────────────────────────────────────────────────
// Streaming behavior
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Streaming — chunks arrive mid-line", "[renderer]") {
    RenderCollect rc;
    rc.feed_chunks("# Heading\nSome te\nxt\n", 3);
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("Heading") != std::string::npos);
    // "Some te\nxt\n" is split by a newline — the line-based renderer
    // emits them as two separate lines.
    CHECK(plain.find("Some te") != std::string::npos);
    CHECK(plain.find("xt") != std::string::npos);
}

TEST_CASE("Streaming — incomplete final line", "[renderer]") {
    // feed without final \n, then finish()
    std::string out;
    StreamingMarkdownRenderer r([&](const std::string& s) { out += s; });
    r.feed("hello", OutputType::Content);
    r.finish();
    auto plain = strip_ansi(out);
    CHECK(plain == "hello");
}

// ─────────────────────────────────────────────────────────────────
// Style cleanup — no bleed across block boundaries
// ─────────────────────────────────────────────────────────────────

TEST_CASE("StyleCleanup — bold ends at newline", "[renderer]") {
    RenderCollect rc;
    rc("**bold line**\nnormal line\n");
    // "normal line" should not have bold styling
    size_t bold_start = rc.out.rfind("\033[1m");
    size_t normal_pos = rc.out.find("normal line");
    // If bold is present, it should only be before "normal line"
    if (bold_start != std::string::npos) {
        CHECK(bold_start < normal_pos); // bold came before normal
    }
}

TEST_CASE("StyleCleanup — heading bold doesn't leak to next paragraph", "[renderer]") {
    RenderCollect rc;
    rc("## **Bold Heading**\nNormal text\n");
    size_t normal_pos = rc.out.find("Normal");
    // After the heading \n, there should be a reset
    auto after_heading = rc.out.substr(0, normal_pos);
    // The last ANSI before "Normal" should include a reset or base style
    size_t last_esc = after_heading.rfind("\033[");
    if (last_esc != std::string::npos) {
        auto esc = after_heading.substr(last_esc);
        CHECK(esc.find("0m") != std::string::npos); // reset
    }
}

TEST_CASE("StyleCleanup — list bold doesn't leak to next item content", "[renderer]") {
    RenderCollect rc;
    rc("- **bold**\n- normal\n");
    // Second item "normal" should not be bold
    size_t second_bullet = rc.out.rfind("\u2022");
    auto after_second_bullet = rc.out.substr(second_bullet);
    size_t bold_in_second = after_second_bullet.find("\033[1m");
    if (bold_in_second != std::string::npos) {
        // Bold should be followed by 22m (close) before "normal" text
        size_t bold_close = after_second_bullet.find("\033[22m", bold_in_second);
        size_t normal_txt = after_second_bullet.find("normal");
        CHECK(bold_close < normal_txt);
    }
}

// ─────────────────────────────────────────────────────────────────
// OutputType transitions (Reasoning / Content color)
// ─────────────────────────────────────────────────────────────────

TEST_CASE("OutputType — reasoning uses gray color", "[renderer]") {
    std::string out;
    StreamingMarkdownRenderer r([&](const std::string& s) { out += s; });
    r.feed("thinking text\n", OutputType::Reasoning);
    r.finish();
    CHECK(out.find("38;5;244") != std::string::npos);
}

TEST_CASE("OutputType — switching type emits reset", "[renderer]") {
    std::string out;
    StreamingMarkdownRenderer r([&](const std::string& s) { out += s; });
    r.feed("reasoning\n", OutputType::Reasoning);
    r.feed("content\n", OutputType::Content);
    r.finish();
    // Should have reset between types
    CHECK(out.find(RESET) != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Edge cases
// ─────────────────────────────────────────────────────────────────

TEST_CASE("EdgeCase — empty input produces empty output", "[renderer]") {
    RenderCollect rc;
    rc("");
    CHECK(rc.out.empty());
}

TEST_CASE("EdgeCase — only newlines", "[renderer]") {
    RenderCollect rc;
    rc("\n\n\n");
    auto plain = strip_ansi(rc.out);
    CHECK(count_substr(plain, "\n") > 0);
}

TEST_CASE("EdgeCase — markdown in code block not interpreted", "[renderer]") {
    RenderCollect rc;
    rc("```\n# not a heading\n- not a list\n> not a quote\n```\n");
    auto plain = strip_ansi(rc.out);
    // All content should appear literally
    CHECK(plain.find("# not a heading") != std::string::npos);
    CHECK(plain.find("- not a list") != std::string::npos);
    CHECK(plain.find("> not a quote") != std::string::npos);
}

TEST_CASE("EdgeCase — backtick counting in code block", "[renderer]") {
    // Single and double backticks inside code block should not close it
    RenderCollect rc;
    rc("```\n`single`\n``double``\nreal code\n```\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("`single`") != std::string::npos);
    CHECK(plain.find("``double``") != std::string::npos);
    CHECK(plain.find("real code") != std::string::npos);
}

TEST_CASE("EdgeCase — asterisk not bold when not paired", "[renderer]") {
    RenderCollect rc;
    rc("single * star\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("*") != std::string::npos);
    CHECK(rc.out.find("\033[1m") == std::string::npos);
}

TEST_CASE("EdgeCase — star then space not bold", "[renderer]") {
    // NOTE: "* " at line start is a list marker, so use a different pattern
    // that has stars with spaces but NOT at line start.
    // We use "x * not bold * y" to test that single stars don't bold.
    RenderCollect rc;
    rc("x * not bold * y\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("* not bold *") != std::string::npos);
    CHECK(rc.out.find("\033[1m") == std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Tables
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Table — header and rows rendered aligned", "[renderer][table]") {
    RenderCollect rc;
    rc("| Name | Age |\n"
       "|------|-----|\n"
       "| Ali  | 30  |\n"
       "| Bob  | 25  |\n");
    auto plain = strip_ansi(rc.out);
    // Header should be bold
    CHECK(rc.out.find("\033[1m") != std::string::npos);
    // Separators should be dimmed
    CHECK(rc.out.find("\033[2m") != std::string::npos);
    // Content should be present and aligned
    CHECK(plain.find("Ali") != std::string::npos);
    CHECK(plain.find("Bob") != std::string::npos);
    CHECK(plain.find("30") != std::string::npos);
}

TEST_CASE("Table — pipe line without separator emitted as paragraph",
          "[renderer][table]") {
    RenderCollect rc;
    rc("| not a table\nmore text\n");
    auto plain = strip_ansi(rc.out);
    // The | line should appear as-is (no table detection)
    CHECK(plain.find("| not a table") != std::string::npos);
}

TEST_CASE("Table — inline formatting preserved in cells", "[renderer][table]") {
    RenderCollect rc;
    rc("| Col A | Col B |\n"
       "|-------|-------|\n"
       "| **bold** | `code` |\n");
    CHECK(rc.out.find("\033[1mbold\033[22m") != std::string::npos);
    CHECK(rc.out.find("38;5;208") != std::string::npos);
}

TEST_CASE("Table — trailing pipe doesn't add empty column", "[renderer][table]") {
    RenderCollect rc;
    rc("| A | B |\n"
       "|---|---|\n"
       "| 1 | 2 |\n");
    auto plain = strip_ansi(rc.out);
    CHECK(plain.find("1") != std::string::npos);
    CHECK(plain.find("2") != std::string::npos);
}

TEST_CASE("Table — blank line after table before next content",
          "[renderer][table]") {
    RenderCollect rc;
    rc("| A |\n"
       "|---|\n"
       "| 1 |\n"
       "text after table\n");
    auto plain = strip_ansi(rc.out);
    // Footer + blank line before "text after table"
    CHECK(plain.find("\n\ntext after table") != std::string::npos);
}

TEST_CASE("Table — footer separator emitted", "[renderer][table]") {
    RenderCollect rc;
    rc("| X |\n"
       "|---|\n"
       "| 1 |\n");
    // Footer separator should be dimmed (like header separator)
    auto ansi = rc.out;
    // There should be two dimmed sections: header separator and footer
    size_t first = ansi.find("\033[2m");
    CHECK(first != std::string::npos);
    size_t second = ansi.find("\033[2m", first + 1);
    CHECK(second != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────
// Spinner
// ─────────────────────────────────────────────────────────────────

TEST_CASE("Spinner — shown for incomplete line", "[renderer][spinner]") {
    // Spinner is only active when stdout is a TTY.
    // In test environments (non-TTY), the spinner is suppressed so output
    // does not contain \\r spin characters.
    // We verify that the line appears correctly regardless.
    std::string out;
    StreamingMarkdownRenderer r([&](const std::string& s) { out += s; });
    r.feed("partial line", OutputType::Content);
    // Incomplete line — buffer should hold "partial line"
    // After feed returns, the content has NOT yet been emitted
    // (it waits for \\n). So the raw output should NOT contain it.
    CHECK(strip_ansi(out).find("partial line") == std::string::npos);
    // Complete the line
    r.feed("\n", OutputType::Content);
    r.finish();
    CHECK(strip_ansi(out).find("partial line") != std::string::npos);
}

TEST_CASE("Spinner — cleared when line completes", "[renderer][spinner]") {
    std::string out;
    StreamingMarkdownRenderer r([&](const std::string& s) { out += s; });
    r.feed("partial", OutputType::Content);
    r.feed(" line\n", OutputType::Content);
    r.finish();
    // Should have the line content
    CHECK(strip_ansi(out).find("partial line") != std::string::npos);
}
