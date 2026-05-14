#include "tools.h"
#include "wiki.h"

#include <sstream>
#include <string>

Tool make_list_wiki_pages_tool(Wiki& wiki) {
    Tool t;
    t.name = "list_wiki_pages";
    t.description =
        "List all wiki page titles. "
        "Returns an array of strings, e.g. [\"page_title1\", \"page_title2\", ...]";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"}, {"properties", json::object()}};
    t.execute = [&wiki](const json& args) -> Result<std::string> {
        (void)args;
        auto result = wiki.list_pages();
        if (!result) {
            return std::unexpected(result.error());
        }
        json arr = json::array();
        for (const auto& title : *result) {
            arr.push_back(title);
        }
        return arr.dump();
    };
    return t;
}

Tool make_read_wiki_page_tool(Wiki& wiki) {
    Tool t;
    t.name = "read_wiki_page";
    t.description =
        "Read a wiki page by title. "
        "Returns the page body as markdown text. "
        "Use offset and max_lines to paginate long pages.";
    t.permission = ToolPermission::ReadOnly;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"page_title",
                 {{"type", "string"}, {"description", "Title of the wiki page to read"}}},
                {"offset",
                    {{"type", "integer"},
                        {"description",
                            "Line number to start from (1-indexed, default 0 = "
                            "beginning)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to read starting from offset (default "
                            "200)"}}}}},
        {"required", {"page_title"}}};
    t.execute = [&wiki](const json& args) -> Result<std::string> {
        auto title = args.value("page_title", std::string());
        if (title.empty()) {
            return std::unexpected("page_title is required");
        }

        auto result = wiki.read_page(title);
        if (!result) {
            return std::unexpected(result.error());
        }

        const std::string& body = *result;

        int offset = args.value("offset", 0);
        int max_lines = args.value("max_lines", 200);
        if (offset < 0)
            offset = 0;
        if (max_lines < 1)
            max_lines = 1;

        // Build full result with line-based pagination
        std::string full_result;
        std::istringstream stream(body);
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before offset
        while (line_num < offset && std::getline(stream, line)) {
            line_num++;
        }

        while (std::getline(stream, line) && count < max_lines) {
            line_num++;
            full_result += line;
            full_result += '\n';
            count++;
        }

        // Check if there are more lines
        bool has_more = stream.peek() != EOF;
        // Count remaining
        int remaining = 0;
        std::string dummy;
        while (std::getline(stream, dummy)) { remaining++; }

        if (has_more || remaining > 0) {
            full_result += "...(truncated, >" + std::to_string(count + remaining) +
                " lines from offset " + std::to_string(offset) + ")";
        } else if (count == 0 && offset > 0) {
            full_result = "(offset " + std::to_string(offset) + " is beyond end of page)";
        }

        return full_result;
    };
    return t;
}

Tool make_write_wiki_page_tool(Wiki& wiki) {
    Tool t;
    t.name = "write_wiki_page";
    t.description =
        "Write a wiki page. Creates the page if it doesn't exist, "
        "or overwrites it if it does.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"page_title",
                 {{"type", "string"}, {"description", "Title of the wiki page"}}},
                {"page_body",
                    {{"type", "string"},
                        {"description", "Markdown content of the page"}}}}},
        {"required", {"page_title", "page_body"}}};
    t.execute = [&wiki](const json& args) -> Result<std::string> {
        auto title = args.value("page_title", std::string());
        auto body = args.value("page_body", std::string());

        if (title.empty()) {
            return std::unexpected("page_title is required");
        }

        auto result = wiki.write_page(title, body);
        if (!result) {
            return std::unexpected(result.error());
        }

        return "ok (" + std::to_string(body.size()) + " bytes written)";
    };
    return t;
}

Tool make_edit_wiki_page_tool(Wiki& wiki) {
    Tool t;
    t.name = "edit_wiki_page";
    t.description =
        "Edit a wiki page by searching for an exact string and replacing it. "
        "The search string must match exactly once in the page body. "
        "Use this to make targeted surgical edits instead of rewriting the entire page.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"page_title",
                 {{"type", "string"}, {"description", "Title of the wiki page to edit"}}},
                {"search",
                    {{"type", "string"},
                        {"description",
                            "Exact string to search for; must match exactly once in "
                            "the page body. Include surrounding context to guarantee a "
                            "single match."}}},
                {"replace",
                    {{"type", "string"},
                        {"description",
                            "String to replace the matched occurrence with"}}}}},
        {"required", {"page_title", "search", "replace"}}};
    t.execute = [&wiki](const json& args) -> Result<std::string> {
        auto title = args.value("page_title", std::string());
        auto search = args.value("search", std::string());
        auto replace = args.value("replace", std::string());

        if (title.empty()) {
            return std::unexpected("page_title is required");
        }
        if (search.empty()) {
            return std::unexpected("search string is required");
        }

        auto result = wiki.edit_page(title, search, replace);
        if (!result) {
            return std::unexpected(result.error());
        }

        return "ok (replaced 1 occurrence, " + std::to_string(search.size()) +
            " bytes -> " + std::to_string(replace.size()) + " bytes)";
    };
    return t;
}

Tool make_delete_wiki_page_tool(Wiki& wiki) {
    Tool t;
    t.name = "delete_wiki_page";
    t.description =
        "Delete a wiki page by title. "
        "Returns \"ok\" on success or \"no such page\" if the title doesn't exist.";
    t.permission = ToolPermission::Write;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"page_title",
                 {{"type", "string"}, {"description", "Title of the wiki page to delete"}}}}},
        {"required", {"page_title"}}};
    t.execute = [&wiki](const json& args) -> Result<std::string> {
        auto title = args.value("page_title", std::string());
        if (title.empty()) {
            return std::unexpected("page_title is required");
        }

        auto result = wiki.delete_page(title);
        if (!result) {
            // Return "no such page" style messages directly
            return std::unexpected(result.error());
        }

        return std::string("ok");
    };
    return t;
}
