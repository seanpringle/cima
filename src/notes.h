#pragma once

#include "tools.h"
#include <map>
#include <string>
#include <vector>

/// Notes — per-agent note storage.
/// Each ChatSession owns one Notes instance. Notes are local to that agent
/// (not shared across tabs like the Wiki).
///
/// Notes use integer IDs assigned sequentially starting from 0.
/// Persistence is handled externally by the session management code,
/// which calls to_json() / from_json() to consolidate data into a
/// single per-assistant JSON file.
class Notes {
  public:
    Notes() = default;

    Notes(const Notes&) = delete;
    Notes& operator=(const Notes&) = delete;
    Notes(Notes&&) = delete;
    Notes& operator=(Notes&&) = delete;

    /// Return all note IDs, sorted ascending.
    Result<std::vector<int>> list_notes();

    /// Read the body of a note. Returns an error if the note does not exist.
    Result<std::string> read_note(int id);

    /// Create or overwrite a note, auto-assigning the next available ID.
    Result<void> write_note(const std::string& body);

    /// Delete a single note by ID. Returns an error if the note does not exist.
    Result<void> delete_note(int id);

    // ── Serialization (used by external persistence) ──

    json to_json() const;
    void from_json(const json& j);

  private:
    std::map<int, std::string> notes_; // id -> body
};

// Tool factory declarations
Tool make_list_notes_tool(Notes& notes);
Tool make_read_note_tool(Notes& notes);
Tool make_write_note_tool(Notes& notes);
Tool make_delete_note_tool(Notes& notes);
