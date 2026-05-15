#pragma once

#include "tools.h"
#include <map>
#include <string>
#include <vector>

/// Notes — per-agent note storage.
/// Each ChatSession owns one Notes instance. Notes are local to that agent
/// (not shared across tabs like the Wiki).
///
/// Unlike the old design, Notes no longer auto-saves to a file on every
/// mutation.  Persistence is handled externally by the session management
/// code, which calls to_json() / from_json() to consolidate data into a
/// single per-assistant JSON file.
class Notes {
  public:
    Notes() = default;

    Notes(const Notes&) = delete;
    Notes& operator=(const Notes&) = delete;
    Notes(Notes&&) = delete;
    Notes& operator=(Notes&&) = delete;

    /// Return all note names, alphabetically sorted.
    Result<std::vector<std::string>> list_all_notes();

    /// Read the body of a note. Returns an error if the note does not exist.
    Result<std::string> read_note(const std::string& name);

    /// Create or overwrite a note.
    Result<void> write_note(const std::string& name, const std::string& body);

    /// Delete a single note. Returns an error if the note does not exist.
    Result<void> delete_note(const std::string& name);

    /// Delete all notes.
    Result<void> delete_all_notes();

    // ── Serialization (used by external persistence) ──

    json to_json() const;
    void from_json(const json& j);

    // ── File persistence (legacy, may be removed) ──

    /// Set the file path for auto-save.
    void set_notes_file_path(const std::string& path) { notes_file_path_ = path; }

    /// Load notes from a JSON file. If the file does not exist or is corrupt,
    /// the notes are left empty (first-run behaviour).
    Result<void> load_from_file(const std::string& path);

    /// Explicitly persist to the configured path (no-op if path is empty).
    /// Called automatically on every mutation.
    Result<void> save();

  private:
    std::map<std::string, std::string> notes_; // name -> body
    std::string notes_file_path_;
};

// Tool factory declarations
Tool make_list_all_notes_tool(Notes& notes);
Tool make_read_note_tool(Notes& notes);
Tool make_write_note_tool(Notes& notes);
Tool make_delete_note_tool(Notes& notes);
Tool make_delete_all_notes_tool(Notes& notes);
