#include "tools.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

Tool make_read_file_lines_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "read_file_lines";
    t.description =
        "Read specific line ranges from a file. Returns lines prefixed with line "
        "numbers. Use this when you know the line numbers you want (e.g. after a "
        "grep match at line 52, read lines 45-78).";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file"}}},
                {"start_line",
                    {{"type", "integer"},
                        {"description",
                            "First line to read (1-indexed, default 1)"}}},
                {"end_line",
                    {{"type", "integer"},
                        {"description",
                            "Last line to read (inclusive). If omitted, reads to "
                            "end of file (capped by max_lines)."}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to return (default 200, max 500)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int start_line = args.value("start_line", 1);
        int end_line = args.value("end_line", 0); // 0 = not specified
        int max_lines = args.value("max_lines", 200);

        if (start_line < 1) {
            return std::unexpected("start_line must be >= 1");
        }
        if (end_line != 0 && end_line < start_line) {
            return std::unexpected("end_line must be >= start_line");
        }
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        std::error_code ec;
        if (std::filesystem::exists(*resolved, ec) &&
            !std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result;
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before start_line
        while (line_num < start_line - 1 && std::getline(file, line)) {
            line_num++;
        }

        // Determine how many lines to read
        int max_to_read = max_lines;
        if (end_line != 0) {
            int range = end_line - start_line + 1;
            if (range < max_to_read) max_to_read = range;
        }

        while (count < max_to_read && std::getline(file, line)) {
            line_num++;
            result += std::to_string(line_num) + ": " + line + "\n";
            count++;
        }

        // Check if there are more lines beyond what was read
        // Try reading one more line to detect EOF reliably
        bool has_more = false;
        int remaining = 0;
        if (end_line != 0 && line_num < end_line) {
            // We haven't read far enough per end_line — definitely has more
            has_more = true;
            std::string dummy;
            while (std::getline(file, dummy)) { remaining++; }
        } else {
            // Check if there's more content by attempting one more read
            std::string peek;
            has_more = static_cast<bool>(std::getline(file, peek));
            if (has_more) {
                remaining = 1;
                std::string dummy;
                while (std::getline(file, dummy)) { remaining++; }
            }
        }

        if (has_more) {
            result += "...(truncated, >" + std::to_string(count + remaining) +
                " lines from line " + std::to_string(start_line) + ")";
        } else if (count == 0 && start_line > 1) {
            result = "(start_line " + std::to_string(start_line) +
                " is beyond end of file)";
        }
        return result;
    };
    return t;
}

Tool make_read_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "read_file";
    t.description = "Read lines from a file (max 400 lines at a time, use offset to "
                    "paginate)";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file to read"}}},
                {"offset",
                    {{"type", "integer"},
                        {"description",
                            "Line number to start from (1-indexed, default 0 = "
                            "beginning)"}}},
                {"max_lines",
                    {{"type", "integer"},
                        {"description",
                            "Maximum lines to read starting from offset (default 200)"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        int offset = args.value("offset", 0);
        int max_lines = args.value("max_lines", 400);
        if (offset < 0)
            offset = 0;
        if (max_lines < 1)
            max_lines = 1;

        std::error_code ec;
        if (std::filesystem::exists(*resolved, ec) &&
            !std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result;
        std::string line;
        int line_num = 0;
        int count = 0;

        // Skip lines before offset
        while (line_num < offset && std::getline(file, line)) {
            line_num++;
        }

        while (std::getline(file, line) && count < max_lines) {
            line_num++;
            result += line;
            result += '\n';
            count++;
        }

        if (!file.eof()) {
            result += "...(truncated, >" + std::to_string(max_lines) + " lines from offset " +
                std::to_string(offset) + ")";
        } else if (count == 0 && offset > 0) {
            result = "(offset " + std::to_string(offset) + " is beyond end of file)";
        }
        return result;
    };
    return t;
}

Tool make_write_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    FileModifiedCallback on_file_modified) {
    Tool t;
    t.name = "write_file";
    t.description = "Write content to a file, creating parent directories if needed";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path"}}},
                {"content", {{"type", "string"}, {"description", "Content to write"}}}}},
        {"required", {"path", "content"}}};
    t.execute = [safe_dir_ptr, on_file_modified](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto content = args.value("content", std::string());

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(*resolved).parent_path(), ec);
        if (ec) {
            return std::unexpected("Failed to create parent directories: " + ec.message());
        }

        std::ofstream file(*resolved, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        file.write(content.data(), content.size());
        file.close();

        // Notify the callback that a file was modified
        if (on_file_modified) {
            on_file_modified(*resolved);
        }

        return "ok (" + std::to_string(content.size()) + " bytes written)";
    };
    return t;
}

Tool make_edit_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    FileModifiedCallback on_file_modified) {
    Tool t;
    t.name = "edit_file";
    t.description = "Edit a file by searching for an exact string and replacing it. "
                    "The search string must match exactly once in the file — this ensures edits "
                    "are safe and unambiguous. "
                    "Use this to make targeted surgical edits instead of rewriting entire files.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path to edit"}}},
                {"search",
                    {{"type", "string"},
                        {"description",
                            "Exact string to search for; must match exactly once in the file. "
                            "Include surrounding context (unique nearby lines) to guarantee a "
                            "single match."}}},
                {"replace",
                    {{"type", "string"},
                        {"description", "String to replace the matched occurrence with"}}}}},
        {"required", {"path", "search", "replace"}}};
    t.execute = [safe_dir_ptr, on_file_modified](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        auto search = args.value("search", std::string());
        auto replace = args.value("replace", std::string());

        if (search.empty()) {
            return std::unexpected(std::string("search string is required"));
        }

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (std::filesystem::exists(*resolved, ec) &&
            !std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        // Read the file
        std::ifstream file(*resolved, std::ios::binary);
        if (!file.is_open()) {
            return std::unexpected("Failed to read file: " + *resolved);
        }
        std::string content(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        // Count occurrences of the search string
        size_t count = 0;
        size_t pos = 0;
        while ((pos = content.find(search, pos)) != std::string::npos) {
            count++;
            pos += search.size();
        }

        if (count == 0) {
            return std::unexpected("Search string not found in file (0 matches).");
        }
        if (count > 1) {
            return std::unexpected("Search string found " + std::to_string(count) +
                " times in file (expected exactly 1). "
                "Include more surrounding context in the search string to uniquely identify the "
                "location.");
        }

        // Locate the unique occurrence
        pos = content.find(search);

        // Replace the search string with the replacement
        content.replace(pos, search.size(), replace);

        // Write the modified content back to the file
        std::ofstream out(*resolved, std::ios::binary);
        if (!out.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        out.write(content.data(), content.size());
        out.close();

        // Notify the callback that a file was modified
        if (on_file_modified) {
            on_file_modified(*resolved);
        }

        // Compute the line number where the edit occurred (1-indexed)
        int line_num = 1;
        for (size_t i = 0; i < pos; i++) {
            if (content[i] == '\n')
                line_num++;
        }

        return "ok (replaced 1 occurrence at line " + std::to_string(line_num) + ", " +
            std::to_string(search.size()) + " bytes -> " + std::to_string(replace.size()) +
            " bytes)";
    };
    return t;
}

Tool make_move_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "move_file";
    t.description = "Move or rename a file from source to destination. "
                    "Works for both same-directory renames and cross-directory moves. "
                    "Will not overwrite an existing destination.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"source", {{"type", "string"}, {"description", "Source file path"}}},
             {"destination", {{"type", "string"}, {"description", "Destination file path"}}}}},
        {"required", {"source", "destination"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto src_raw = args.value("source", std::string());
        auto dst_raw = args.value("destination", std::string());

        if (src_raw.empty()) {
            return std::unexpected(std::string("source is required"));
        }
        if (dst_raw.empty()) {
            return std::unexpected(std::string("destination is required"));
        }

        auto src = resolve_path(src_raw, *safe_dir_ptr);
        if (!src) return std::unexpected(src.error());
        auto dst = resolve_path(dst_raw, *safe_dir_ptr);
        if (!dst) return std::unexpected(dst.error());

        std::error_code ec;
        if (!std::filesystem::exists(*src, ec)) {
            return std::unexpected("Source not found: " + *src);
        }
        if (!std::filesystem::is_regular_file(*src, ec)) {
            return std::unexpected("Source is not a regular file: " + *src);
        }
        if (std::filesystem::exists(*dst, ec)) {
            return std::unexpected("Destination already exists: " + *dst);
        }

        // Create parent directories of destination
        std::filesystem::create_directories(std::filesystem::path(*dst).parent_path(), ec);
        if (ec) {
            return std::unexpected(
                "Failed to create parent directories: " + ec.message());
        }

        std::filesystem::rename(*src, *dst, ec);
        if (ec) {
            // Cross-device link: fall back to copy + delete
            if (ec.value() == EXDEV) {
                std::filesystem::copy_file(*src, *dst, std::filesystem::copy_options::none, ec);
                if (ec) {
                    return std::unexpected(
                        "Failed to copy across devices: " + ec.message());
                }
                std::filesystem::remove(*src, ec);
                if (ec) {
                    // Clean up destination
                    std::filesystem::remove(*dst, ec);
                    return std::unexpected(
                        "Failed to remove source after copy: " + ec.message());
                }
            } else {
                return std::unexpected("Failed to move file: " + ec.message());
            }
        }

        return "ok (moved " + *src + " \u2192 " + *dst + ")";
    };
    return t;
}
