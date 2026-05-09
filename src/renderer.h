#pragma once

#include "chat.h"

#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────
// StreamingMarkdownRenderer — line-buffered streaming renderer
//
// Accumulates characters into a line buffer; when \n arrives (or
// the stream ends), parses the complete line for block constructs
// (headings, lists, code fences, blockquotes, HRs) and emits
// ANSI-styled output with blank-line separation between block types.
//
// A spinner is shown while a line is incomplete for responsiveness.
// ─────────────────────────────────────────────────────────────────

class StreamingMarkdownRenderer {
public:
  using WriteFn = std::function<void(const std::string&)>;

  explicit StreamingMarkdownRenderer(WriteFn write);

  void feed(const std::string& text, OutputType type);
  void flush();
  void finish();

private:
  // ── block types (for spacing logic) ──
  enum class BlockType : uint8_t {
    None,
    Paragraph,
    Heading,
    HR,
    ListItem,
    CodeBlock,
    Blockquote,
    Table,
  };

  // ── line buffer ──
  std::string line_buf_;

  // ── block spacing state ──
  BlockType prev_type_ = BlockType::None;
  bool needs_para_sep_ = false;
  bool ever_emitted_ = false;
  bool just_emitted_blank_ = false;

  // ── code block state ──
  bool in_code_block_ = false;
  std::string code_lang_;

  // ── list state ──
  struct ListLevel {
    int depth = 0;
    bool ordered = false;
    int counter = 0;
  };
  std::vector<ListLevel> list_stack_;

  // ── table state ──
  enum class TableState : uint8_t { None, Header, Collecting };
  TableState table_state_ = TableState::None;
  std::vector<std::string> table_lines_; // header + rows

  // ── output ──
  WriteFn write_;
  int columns_ = 80;
  OutputType current_type_ = OutputType::Content;

  // ── spinner ──
  mutable int spin_idx_ = 0;
  mutable int spin_throttle_ = 0;
  mutable bool spinner_visible_ = false;
  bool use_spinner_ = false;
  bool cursor_hidden_ = false;
  bool last_raw_newline_ = true;

  // ── raw output ──
  void raw(const std::string& s);

  // ── ANSI helpers ──
  void begin_bold();
  void end_bold();
  void begin_inline_code();
  void end_inline_code();
  void apply_block_style(BlockType type, int heading_level = 0);

  // ── type transition ──
  void apply_type(OutputType type);

  // ── line processing ──
  void process_line(const std::string& line, bool trailing_newline = true);
  void emit_blank_if_needed(BlockType new_type);
  BlockType detect_block(const std::string& line, int& heading_level, int& list_depth, bool& list_ordered, int& list_number, std::string& content);
  void emit_block(BlockType type, const std::string& content, int heading_level, int list_depth, bool list_ordered, int list_number, bool trailing_newline);
  void emit_inline(const std::string& text);
  void emit_code_line(const std::string& line);
  void emit_list_continuation(const std::string& line);

  // ── table ──
  bool is_table_separator(const std::string& line);
  bool is_table_row(const std::string& line);
  std::vector<std::string> split_table_cells(const std::string& line);
  int visible_width(const std::string& text);
  void emit_table();

  // ── spinner ──
  void show_spinner();
  void hide_spinner();
};
