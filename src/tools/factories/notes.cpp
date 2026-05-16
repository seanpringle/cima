#include "notes.h"
#include "tools.h"

#include <sstream>
#include <string>

// ===================================================================
// Tool: list_notes
// ===================================================================

Tool make_list_notes_tool(Notes& notes) {
    Tool t;
    t.name = "list_notes";
    t.description =
        "List all note IDs. "
        "Returns an array of integers, e.g. [0, 1, 2]";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        (void)args;
        auto result = notes.list_notes();
        if (!result) {
            return std::unexpected(result.error());
        }
        json arr = json::array();
        for (int id : *result) {
            arr.push_back(id);
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
        "Read a note by ID. "
        "Returns the note body as text. "
        "Returns an error if the note does not exist.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"note_id",
                {{"type", "integer"}, {"description", "ID of the note to read"}}}}},
        {"required", {"note_id"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        int id = args.value("note_id", -1);
        auto result = notes.read_note(id);
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
        "Create or overwrite a note. "
        "If note_id is omitted, the next available ID is auto-assigned.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"note_id",
                 {{"type", "integer"},
                    {"description",
                        "Note ID. Omitted to auto-assign the next available ID"}}},
                {"body",
                    {{"type", "string"},
                        {"description", "Body content of the note"}}}}},
        {"required", {"body"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        auto body = args.value("body", std::string());

        auto result = notes.write_note(body);
        if (!result) {
            return std::unexpected(result.error());
        }
        auto ids = notes.list_notes();
        int id = ids ? static_cast<int>(ids->size()) - 1 : 0;
        return "ok (" + std::to_string(body.size()) + " bytes written to note #" +
            std::to_string(id) + ")";
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
        "Delete a note by ID. "
        "Returns an error if the note does not exist.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"note_id",
                {{"type", "integer"}, {"description", "ID of the note to delete"}}}}},
        {"required", {"note_id"}}};
    t.execute = [&notes](const json& args) -> Result<std::string> {
        int id = args.value("note_id", -1);
        auto result = notes.delete_note(id);
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::string("ok");
    };
    return t;
}
