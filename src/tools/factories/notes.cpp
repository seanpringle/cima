#include "notes.h"
#include "tools.h"

#include <sstream>
#include <string>

// ===================================================================
// Tool: list_all_notes
// ===================================================================

Tool make_list_all_notes_tool(Notes& notes) {
    Tool t;
    t.name = "list_all_notes";
    t.description =
        "List all note names. "
        "Returns an array of strings, e.g. [\"name1\", \"name2\", ...]";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        (void)args;
        auto result = notes.list_all_notes();
        if (!result) {
            return std::unexpected(result.error());
        }
        json arr = json::array();
        for (const auto& name : *result) {
            arr.push_back(name);
        }
        return arr.dump();
    };
    return t;
}

// ===================================================================
// Tool: read_note
// ===================================================================

Tool make_read_note_tool(Notes& notes) {
    Tool t;
    t.name = "read_note";
    t.description =
        "Read a note by name. "
        "Returns the note body as text. "
        "Returns an error if the note does not exist.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"}, {"description", "Name of the note to read"}}}}},
        {"required", {"name"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        if (name.empty()) {
            return std::unexpected("name is required");
        }
        auto result = notes.read_note(name);
        if (!result) {
            return std::unexpected(result.error());
        }
        return *result;
    };
    return t;
}

// ===================================================================
// Tool: write_note
// ===================================================================

Tool make_write_note_tool(Notes& notes) {
    Tool t;
    t.name = "write_note";
    t.description =
        "Write a note. Creates the note if it doesn't exist, "
        "or overwrites it if it does.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name", {{"type", "string"}, {"description", "Name of the note"}}},
                {"body",
                    {{"type", "string"},
                        {"description", "Body content of the note"}}}}},
        {"required", {"name", "body"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        auto body = args.value("body", std::string());
        if (name.empty()) {
            return std::unexpected("name is required");
        }
        auto result = notes.write_note(name, body);
        if (!result) {
            return std::unexpected(result.error());
        }
        return "ok (" + std::to_string(body.size()) + " bytes written)";
    };
    return t;
}

// ===================================================================
// Tool: delete_note
// ===================================================================

Tool make_delete_note_tool(Notes& notes) {
    Tool t;
    t.name = "delete_note";
    t.description =
        "Delete a note by name. "
        "Returns an error if the note does not exist.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"name",
                {{"type", "string"}, {"description", "Name of the note to delete"}}}}},
        {"required", {"name"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        auto name = args.value("name", std::string());
        if (name.empty()) {
            return std::unexpected("name is required");
        }
        auto result = notes.delete_note(name);
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::string("ok");
    };
    return t;
}

// ===================================================================
// Tool: delete_all_notes
// ===================================================================

Tool make_delete_all_notes_tool(Notes& notes) {
    Tool t;
    t.name = "delete_all_notes";
    t.description =
        "Delete every note. "
        "Clears all notes for this assistant session.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"}, {"properties", json::object()}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        (void)args;
        auto result = notes.delete_all_notes();
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::string("ok (all notes deleted)");
    };
    return t;
}
