#include "renderer.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <clocale>
#include <cwchar>
#include <string_view>
#include <sys/ioctl.h>
#include <unistd.h>

// ─────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────

StreamingMarkdownRenderer::StreamingMarkdownRenderer(WriteFn write) : write_(std::move(write)) {
  std::setlocale(LC_CTYPE, ""); // needed by mbtowc / wcwidth
  struct winsize w;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
    columns_ = w.ws_col;
  } else {
    const char* env = std::getenv("COLUMNS");
    columns_ = env ? std::atoi(env) : 80;
    if (columns_ < 20)
      columns_ = 80;
  }
  use_spinner_ = isatty(STDOUT_FILENO);
}

// ─────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::feed(const std::string& text, OutputType type) {
  hide_spinner();

  // Handle type transition
  if (type != current_type_) {
    if (type == OutputType::ToolInvocation) {
      raw(text);
      return;
    }
    if (text.empty()) {
      // Empty chunk on type change — don't trigger transition side
      // effects, just update the type silently.
      current_type_ = type;
      return;
    }
    flush();
    apply_type(type);
  }

  if (text.empty())
    return;

  // Split on \n, process complete lines, buffer remainder
  size_t pos = 0;
  size_t next;
  while ((next = text.find('\n', pos)) != std::string::npos) {
    line_buf_ += text.substr(pos, next - pos);
    process_line(line_buf_, true);
    line_buf_.clear();
    pos = next + 1;
  }
  // Remainder (no trailing \n)
  if (pos < text.size()) {
    line_buf_ += text.substr(pos);
  }

  if (!line_buf_.empty()) {
    show_spinner();
  }
}

void StreamingMarkdownRenderer::flush() {
  hide_spinner();
  if (!line_buf_.empty()) {
    process_line(line_buf_, false);
    line_buf_.clear();
  }
  // Emit any buffered table that wasn't terminated by a non-| line
  if (table_state_ != TableState::None) {
    emit_table();
  }
}

void StreamingMarkdownRenderer::finish() {
  flush();
  // emit_table is already called by flush(), but guard anyway
  if (table_state_ != TableState::None) {
    emit_table();
  }
  hide_spinner();
  if (cursor_hidden_) {
    raw("\033[?25h");
    cursor_hidden_ = false;
  }
  if (ever_emitted_)
    raw("\033[0m");
}

// ─────────────────────────────────────────────────────────────────
// Raw output
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::raw(const std::string& s) {
  if (write_) {
    write_(s);
    if (!s.empty())
      last_raw_newline_ = (s.back() == '\n');
  }
}

// ─────────────────────────────────────────────────────────────────
// ANSI helpers
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::begin_bold() { raw("\033[1m"); }
void StreamingMarkdownRenderer::end_bold() { raw("\033[22m"); }
void StreamingMarkdownRenderer::begin_inline_code() { raw("\033[38;5;208m"); }
void StreamingMarkdownRenderer::end_inline_code() { raw("\033[39m"); }

void StreamingMarkdownRenderer::apply_block_style(BlockType type, int heading_level) {
  switch (type) {
  case BlockType::Heading: {
    static const char* colors[] = {
        "38;5;220",
        "38;5;215",
        "38;5;117",
        "38;5;183",
        "38;5;152",
        "38;5;145",
    };
    int idx = std::clamp(heading_level - 1, 0, 5);
    raw("\033[" + std::string(colors[idx]) + "m");
    break;
  }
  case BlockType::Blockquote:
    raw("\033[2m\033[38;5;245m");
    break;
  case BlockType::CodeBlock:
    raw("\033[48;5;236m\033[38;5;208m");
    break;
  default:
    break;
  }
}

void StreamingMarkdownRenderer::apply_type(OutputType type) {
  // Move to a fresh line if the last raw emission didn't end with one
  // (e.g. after flush() of an incomplete line), so the spinner won't
  // overwrite previous content.
  if (!last_raw_newline_)
    raw("\n");
  raw("\033[0m");
  current_type_ = type;
  if (type == OutputType::Reasoning) {
    raw("\033[38;5;244m");
  }
}

// ─────────────────────────────────────────────────────────────────
// Block detection
// ─────────────────────────────────────────────────────────────────

StreamingMarkdownRenderer::BlockType
StreamingMarkdownRenderer::detect_block(const std::string& line, int& heading_level, int& list_depth, bool& list_ordered, int& list_number, std::string& content) {
  auto trimmed = std::string_view(line);

  // Strip trailing \r if present
  if (!trimmed.empty() && trimmed.back() == '\r')
    trimmed.remove_suffix(1);

  // Blank line
  if (trimmed.empty() || std::all_of(trimmed.begin(), trimmed.end(), [](char c) { return c == ' ' || c == '\t'; })) {
    return BlockType::None;
  }

  // Code fence: ```lang
  if (trimmed.size() >= 3 && trimmed[0] == '`' && trimmed[1] == '`' && trimmed[2] == '`') {
    // Extract language tag
    code_lang_ = std::string(trimmed.substr(3));
    return BlockType::CodeBlock;
  }

  // HR: ---, ***, ___
  {
    auto sv = trimmed;
    if (sv.size() >= 3) {
      char c = sv[0];
      if (c == '-' || c == '*' || c == '_') {
        bool all_same = true;
        for (size_t i = 1; i < sv.size(); i++) {
          if (sv[i] != c) {
            all_same = false;
            break;
          }
        }
        if (all_same) {
          return BlockType::HR;
        }
      }
    }
  }

  // Heading: #{1,6} + space + content
  {
    auto sv = trimmed;
    int hash_count = 0;
    while (hash_count < 6 && hash_count < (int)sv.size() && sv[hash_count] == '#')
      hash_count++;
    if (hash_count >= 1 && hash_count <= 6 && (int)sv.size() > hash_count && sv[hash_count] == ' ') {
      heading_level = hash_count;
      content = std::string(sv.substr(hash_count + 1));
      return BlockType::Heading;
    }
  }

  // List: leading spaces, then marker, then space, then content
  {
    auto sv = trimmed;
    size_t spaces = 0;
    while (spaces < sv.size() && sv[spaces] == ' ')
      spaces++;

    if (spaces + 2 <= sv.size() && sv[spaces + 1] == ' ') {
      char marker = sv[spaces];
      if (marker == '-' || marker == '*' || marker == '+') {
        list_depth = (int)spaces / 2;
        list_ordered = false;
        content = std::string(sv.substr(spaces + 2));
        return BlockType::ListItem;
      }
    }

    // Ordered list: digits + . or ) + space + content
    if (spaces < sv.size() && std::isdigit(static_cast<unsigned char>(sv[spaces]))) {
      size_t num_end = spaces;
      while (num_end < sv.size() && std::isdigit(static_cast<unsigned char>(sv[num_end])))
        num_end++;
      if (num_end < sv.size() && (sv[num_end] == '.' || sv[num_end] == ')') && num_end + 1 < sv.size() && sv[num_end + 1] == ' ') {
        list_depth = (int)spaces / 2;
        list_ordered = true;
        list_number = std::stoi(std::string(sv.substr(spaces, num_end - spaces)));
        content = std::string(sv.substr(num_end + 2));
        return BlockType::ListItem;
      }
    }
  }

  // Blockquote: > content
  {
    auto sv = trimmed;
    int quote_depth = 0;
    while (quote_depth < (int)sv.size() && sv[quote_depth] == '>')
      quote_depth++;
    if (quote_depth > 0) {
      if (quote_depth < (int)sv.size() && sv[quote_depth] == ' ')
        quote_depth++;
      content = std::string(sv.substr(quote_depth));
      return BlockType::Blockquote;
    }
  }

  // Paragraph
  content = std::string(trimmed);
  return BlockType::Paragraph;
}

// ─────────────────────────────────────────────────────────────────
// Block spacing
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::emit_blank_if_needed(BlockType new_type) {
  if (!needs_para_sep_ || !ever_emitted_)
    return;

  // Content → Content: no blank line
  if (prev_type_ == BlockType::Paragraph && new_type == BlockType::Paragraph)
    return;

  // Same-type block continuations: no blank line
  if (prev_type_ == BlockType::ListItem && new_type == BlockType::ListItem)
    return;
  if (prev_type_ == BlockType::Blockquote && new_type == BlockType::Blockquote)
    return;

  raw("\n");
  needs_para_sep_ = false;
}

// ─────────────────────────────────────────────────────────────────
// Line processing — main dispatcher
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::process_line(const std::string& line, bool trailing_newline) {
  // Inside code block: every line is code until closing fence
  if (in_code_block_) {
    auto trimmed = std::string_view(line);
    if (!trimmed.empty() && trimmed.back() == '\r')
      trimmed.remove_suffix(1);
    if (trimmed == "```") {
      // Trailing padding line inside the code block
      raw("\033[48;5;236m");
      raw(std::string(columns_, ' '));
      raw("\033[0m\n");
      // Closing fence
      in_code_block_ = false;
      raw("\033[0m");
      raw("\033[2m");
      for (int i = 0; i < columns_; i++)
        raw("\u2550");
      raw("\033[0m\n");
      prev_type_ = BlockType::CodeBlock;
      needs_para_sep_ = true;
      ever_emitted_ = true;
    } else {
      emit_code_line(line);
    }
    return;
  }

  // ── Table state machine ──
  // Buffer lines that start with | until we can confirm they form a
  // table (header followed by separator) or should be emitted as-is.
  if (!in_code_block_ && table_state_ != TableState::None) {
    if (is_table_separator(line)) {
      if (table_state_ == TableState::Header) {
        table_lines_.push_back(line);
        table_state_ = TableState::Collecting;
      }
      return;
    }
    if (is_table_row(line)) {
      if (table_state_ == TableState::Collecting) {
        table_lines_.push_back(line);
        return;
      }
      // First | line, but we were already in Header from a previous
      // | line that wasn't confirmed.  Emit the old one first.
      emit_table();
      table_lines_.push_back(line);
      table_state_ = TableState::Header;
      return;
    }
    // Non-table line — finalise any buffered table
    emit_table();
    // fall through to normal block handling
  }

  // Fresh table detection: line starts with |
  if (!in_code_block_ && is_table_row(line)) {
    table_lines_.push_back(line);
    table_state_ = TableState::Header;
    return;
  }

  // Check for list continuation (indented paragraph text without a new
  // block marker). Indented lines that ARE block markers (list items,
  // blockquotes, etc.) fall through to normal block handling.
  if (!list_stack_.empty() && !line.empty() && line[0] == ' ') {
    int heading_level, list_depth, list_number;
    bool list_ordered;
    std::string content;
    BlockType bt = detect_block(line, heading_level, list_depth, list_ordered, list_number, content);
    // Only continue if the indented line is NOT a block element.
    // List items, blockquotes, headings etc. at any indent are handled
    // as new blocks.  Blank lines fall through to end list separation.
    if (bt == BlockType::Paragraph) {
      emit_list_continuation(line);
      return;
    }
  }

  // Detect block type
  int heading_level = 0;
  int list_depth = 0;
  int list_number = 1;
  bool list_ordered = false;
  std::string content;
  BlockType bt = detect_block(line, heading_level, list_depth, list_ordered, list_number, content);

  // Blank line — emit but collapse consecutive blanks to one
  if (bt == BlockType::None) {
    if (!just_emitted_blank_) {
      raw("\n");
      just_emitted_blank_ = true;
    }
    needs_para_sep_ = false;
    return;
  }
  just_emitted_blank_ = false;

  // Insert blank line for transitions (before emitting the new block)
  emit_blank_if_needed(bt);

  // Emit the block
  emit_block(bt, content, heading_level, list_depth, list_ordered, list_number, trailing_newline);

  // Update state
  prev_type_ = bt;
  if (bt == BlockType::Paragraph && content.empty())
    prev_type_ = BlockType::None; // treat empty line as None
  needs_para_sep_ = true;
  ever_emitted_ = true;
}

// ─────────────────────────────────────────────────────────────────
// Block emission
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::emit_block(
    BlockType type, const std::string& content, int heading_level, int list_depth, bool list_ordered, int list_number, bool trailing_newline) {
  switch (type) {
  case BlockType::Heading:
    apply_block_style(type, heading_level);
    emit_inline(content);
    raw("\033[0m\n");
    break;

  case BlockType::HR:
    raw("\033[2m");
    for (int i = 0; i < columns_ && i < 80; i++)
      raw("\u2500");
    raw("\033[0m\n");
    needs_para_sep_ = true;
    break;

  case BlockType::ListItem: {
    // Manage list stack
    if (!list_stack_.empty() && list_depth == list_stack_.back().depth && list_ordered == list_stack_.back().ordered) {
      // Same level, same type — increment counter
      if (list_ordered)
        list_stack_.back().counter++;
    } else if (!list_stack_.empty() && list_stack_.back().depth < list_depth) {
      // Deeper — push new level
      list_stack_.push_back({list_depth, list_ordered, list_ordered ? 1 : -1});
    } else {
      // New list or higher level
      while (!list_stack_.empty() && list_stack_.back().depth >= list_depth)
        list_stack_.pop_back();
      list_stack_.push_back({list_depth, list_ordered, list_ordered ? 1 : -1});
    }

    auto& cur = list_stack_.back();

    // Emit indent
    int indent = list_depth * 2;
    std::string prefix(indent, ' ');

    // Emit bullet
    apply_block_style(BlockType::None, 0);
    if (list_ordered) {
      prefix += std::to_string(cur.counter) + ". ";
    } else {
      prefix += "\u2022 ";
    }
    raw(prefix);

    // Emit content with inline formatting
    emit_inline(content);
    raw("\033[0m\n");
    break;
  }

  case BlockType::Blockquote:
    apply_block_style(type, 0);
    raw("\u2502 ");
    emit_inline(content);
    raw("\033[0m\n");
    break;

  case BlockType::CodeBlock:
    // Opening fence
    in_code_block_ = true;
    raw("\033[2m");
    raw("\u2550\u2550\u2550");
    if (!code_lang_.empty()) {
      raw(" " + code_lang_ + " ");
    } else {
      raw(" ");
    }
    for (int i = 0; i < columns_ - 4 - (int)code_lang_.size() - 1; i++)
      raw("\u2550");
    raw("\033[0m\n");
    // Leading padding line inside the code block
    raw("\033[48;5;236m");
    raw(std::string(columns_, ' '));
    raw("\033[0m\n");
    break;

  case BlockType::Paragraph:
    apply_block_style(BlockType::None, 0);
    emit_inline(content);
    if (trailing_newline)
      raw("\n");
    break;

  case BlockType::None:
    break;
  }
}

// ─────────────────────────────────────────────────────────────────
// Inline formatting
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::emit_inline(const std::string& text) {
  bool in_bold = false;
  bool in_code = false;
  bool escape = false;

  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];

    if (escape) {
      escape = false;
      raw(std::string(1, c));
      continue;
    }
    if (c == '\\') {
      escape = true;
      continue;
    }

    if (c == '`') {
      if (in_code) {
        in_code = false;
        end_inline_code();
      } else {
        in_code = true;
        begin_inline_code();
      }
      continue;
    }

    if (c == '*' && i + 1 < text.size() && text[i + 1] == '*') {
      if (in_bold) {
        in_bold = false;
        end_bold();
      } else {
        in_bold = true;
        begin_bold();
      }
      i++; // skip second *
      continue;
    }

    raw(std::string(1, c));
  }

  // Close any unclosed inline styles at end of line
  if (in_bold)
    end_bold();
  if (in_code)
    end_inline_code();
  if (escape)
    raw("\\");
}

// ─────────────────────────────────────────────────────────────────
// Code block content emission
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::emit_code_line(const std::string& line) {
  // Strip trailing \r if present
  std::string_view sv(line);
  if (!sv.empty() && sv.back() == '\r')
    sv.remove_suffix(1);

  std::string content(sv);

  raw("\033[48;5;236m\033[38;5;208m");

  // Left padding: 2 spaces
  raw("  ");
  raw(content);

  // Right padding to terminal width
  int visible = 2 + (int)content.size();
  if (visible < columns_)
    raw(std::string(columns_ - visible, ' '));

  raw("\033[0m\n");
}

// ─────────────────────────────────────────────────────────────────
// List continuation (indented paragraph within a list item)
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::emit_list_continuation(const std::string& line) {
  int depth = list_stack_.back().depth;
  int indent_size = (depth + 1) * 2; // indent to content level

  // Count leading spaces in input
  size_t leading = 0;
  while (leading < line.size() && line[leading] == ' ')
    leading++;

  std::string content = line.substr(leading);

  std::string prefix(indent_size, ' ');
  raw(prefix);
  emit_inline(content);
  raw("\n");
}

// ─────────────────────────────────────────────────────────────────
// Table support
// ─────────────────────────────────────────────────────────────────

bool StreamingMarkdownRenderer::is_table_separator(const std::string& line) {
  // Must start and end with |, and contain at least one --- cell
  auto sv = std::string_view(line);
  if (sv.empty() || sv.front() != '|' || sv.back() != '|')
    return false;
  // All characters must be |, -, :, or whitespace
  bool has_dashes = false;
  for (char c : sv) {
    if (c == '|' || c == '-' || c == ':' || c == ' ') {
      if (c == '-')
        has_dashes = true;
    } else
      return false;
  }
  return has_dashes;
}

bool StreamingMarkdownRenderer::is_table_row(const std::string& line) {
  auto sv = std::string_view(line);
  return !sv.empty() && sv.front() == '|' && sv.back() == '|' && !is_table_separator(line);
}

std::vector<std::string> StreamingMarkdownRenderer::split_table_cells(const std::string& line) {
  std::vector<std::string> cells;
  auto sv = std::string_view(line);
  if (!sv.empty() && sv.front() == '|')
    sv.remove_prefix(1);
  if (!sv.empty() && sv.back() == '|')
    sv.remove_suffix(1);

  size_t pos = 0;
  while (pos <= sv.size()) {
    size_t next = sv.find('|', pos);
    auto cell = std::string(sv.substr(pos, next - pos));
    // Trim surrounding whitespace
    size_t start = 0, end = cell.size();
    while (start < end && cell[start] == ' ')
      start++;
    while (end > start && cell[end - 1] == ' ')
      end--;
    cells.push_back(cell.substr(start, end - start));
    if (next == std::string_view::npos)
      break;
    pos = next + 1;
  }
  // Drop trailing empty cell (from trailing | in source)
  if (!cells.empty() && cells.back().empty())
    cells.pop_back();
  return cells;
}

int StreamingMarkdownRenderer::visible_width(const std::string& text) {
  int w = 0;
  bool escape = false;
  size_t i = 0;
  while (i < text.size()) {
    unsigned char c = static_cast<unsigned char>(text[i]);

    if (escape) {
      escape = false;
      i++;
      w++;
      continue;
    }
    if (c == '\\') {
      escape = true;
      i++;
      continue;
    }
    // Bold markers ** — consume both chars, no visible width
    if (c == '*' && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == '*') {
      i += 2;
      continue;
    }
    // Inline code markers ` — consume, no visible width
    if (c == '`') {
      i++;
      continue;
    }

    // Multi-byte UTF-8: use wcwidth for display width
    if (c >= 0x80) {
      char buf[8] = {};
      int len = 0;
      if (c < 0xE0)
        len = 2;
      else if (c < 0xF0)
        len = 3;
      else
        len = 4;
      if (i + len <= text.size()) {
        std::memcpy(buf, text.data() + i, len);
        wchar_t wc = 0;
        std::mbtowc(&wc, buf, len);
        int cw = wcwidth(wc);
        w += (cw > 0) ? cw : 1;
      } else {
        w++;
      }
      i += len;
    } else {
      w++;
      i++;
    }
  }
  if (escape)
    w++; // trailing backslash renders as literal
  return w;
}

void StreamingMarkdownRenderer::emit_table() {
  if (table_lines_.empty()) {
    table_state_ = TableState::None;
    return;
  }

  // We need at least 2 lines: header + separator
  if (table_lines_.size() < 2 || !is_table_separator(table_lines_[1])) {
    // Not a table — emit the buffered lines as paragraphs
    for (auto& ln : table_lines_)
      emit_block(BlockType::Paragraph, ln, 0, 0, false, 0, true);
    table_lines_.clear();
    table_state_ = TableState::None;
    return;
  }

  // Column widths: determined from header and all data rows
  auto header_cells = split_table_cells(table_lines_[0]);
  size_t ncols = header_cells.size();

  std::vector<int> col_widths(ncols, 0);
  for (size_t i = 0; i < ncols; i++)
    col_widths[i] = visible_width(header_cells[i]);

  // Scan all rows (skip separator at index 1)
  std::vector<std::vector<std::string>> rows;
  for (size_t r = 2; r < table_lines_.size(); r++) {
    auto cells = split_table_cells(table_lines_[r]);
    while (cells.size() < ncols)
      cells.push_back("");
    rows.push_back(std::move(cells));
    for (size_t i = 0; i < ncols && i < rows.back().size(); i++)
      col_widths[i] = std::max(col_widths[i], visible_width(rows.back()[i]));
  }

  // Minimum column width: 3
  for (auto& w : col_widths)
    w = std::max(w, 3);

  // Emit separator line (dimmed)
  raw("\033[2m");
  for (size_t i = 0; i < ncols; i++) {
    raw("|");
    raw(std::string(col_widths[i] + 2, '-'));
  }
  raw("|\033[0m\n");

  // Emit header row (bold)
  raw("\033[1m");
  for (size_t i = 0; i < ncols; i++) {
    raw("| ");
    int before = visible_width(header_cells[i]);
    emit_inline(header_cells[i]);
    int pad = col_widths[i] - before;
    raw(std::string(pad + 1, ' '));
  }
  raw("|\033[22m\n");

  // Emit separator again
  raw("\033[2m");
  for (size_t i = 0; i < ncols; i++) {
    raw("|");
    raw(std::string(col_widths[i] + 2, '-'));
  }
  raw("|\033[0m\n");

  // Emit data rows
  for (auto& row : rows) {
    for (size_t i = 0; i < ncols; i++) {
      raw("| ");
      std::string cell = i < row.size() ? row[i] : "";
      int before = visible_width(cell);
      emit_inline(cell);
      int pad = col_widths[i] - before;
      raw(std::string(pad + 1, ' '));
    }
    raw("|\n");
  }

  // Emit closing footer separator (dimmed, same style as header)
  raw("\033[2m");
  for (size_t i = 0; i < ncols; i++) {
    raw("|");
    raw(std::string(col_widths[i] + 2, '-'));
  }
  raw("|\033[0m\n");

  // Trailing blank line — emit now so it always appears, even when
  // the table is the last block (no subsequent line to trigger
  // emit_blank_if_needed).  Clear needs_para_sep_ so the next block
  // won't double the blank.
  raw("\n");
  needs_para_sep_ = false;

  // Update spacing state
  prev_type_ = BlockType::Table;
  ever_emitted_ = true;

  table_lines_.clear();
  table_state_ = TableState::None;
}

// ─────────────────────────────────────────────────────────────────
// Spinner
// ─────────────────────────────────────────────────────────────────

void StreamingMarkdownRenderer::show_spinner() {
  if (!use_spinner_)
    return;

  if (!cursor_hidden_) {
    raw("\033[?25l");
    cursor_hidden_ = true;
  }

  // Show immediately the first time we start buffering; throttle
  // subsequent updates so the spinner is calm but still responsive.
  bool first_time = !spinner_visible_;
  spin_throttle_++;
  if (!first_time && spin_throttle_ % 4 != 0)
    return;

  static const char spin[] = "|/-\\";
  char c = spin[spin_idx_ % 4];
  spin_idx_++;
  raw("\r" + std::string(1, c));
  spinner_visible_ = true;
}

void StreamingMarkdownRenderer::hide_spinner() {
  if (!use_spinner_)
    return;

  if (spinner_visible_) {
    raw("\r \r");
    spinner_visible_ = false;
  }

  // Always restore cursor when clearing spinner area
  if (cursor_hidden_) {
    raw("\033[?25h");
    cursor_hidden_ = false;
  }
}
