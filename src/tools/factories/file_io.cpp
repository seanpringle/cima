#include "tools.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

Tool make_stat_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths) {
    Tool t;
    t.name = "stat_file";
    t.description = "Get metadata about a file or directory. "
                    "Returns type, size, permissions, and timestamps.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file or directory"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr, read_only_paths](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());

        // Check for symlink on the raw path *before* resolve_path canonicalizes it.
        // resolve_path uses weakly_canonical which follows symlinks, so we need
        // to detect symlinks first.
        std::filesystem::path pre_path(raw);
        if (pre_path.is_relative()) {
            pre_path = std::filesystem::path(*safe_dir_ptr) / pre_path;
        }

        std::error_code ec;
        bool is_symlink = false;
        auto pre_st = std::filesystem::symlink_status(pre_path, ec);
        if (!ec && pre_st.type() == std::filesystem::file_type::symlink) {
            is_symlink = true;
        }

        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        // Get the target's status (follows symlinks)
        auto s = std::filesystem::status(*resolved, ec);
        if (ec) {
            return std::unexpected("Cannot access path: " + ec.message());
        }

        json j;
        auto ft = s.type();
        if (is_symlink)
            j["type"] = "symlink";
        else if (ft == std::filesystem::file_type::regular)
            j["type"] = "regular_file";
        else if (ft == std::filesystem::file_type::directory)
            j["type"] = "directory";
        else if (ft == std::filesystem::file_type::symlink)
            j["type"] = "symlink";
        else if (ft == std::filesystem::file_type::block)
            j["type"] = "block";
        else if (ft == std::filesystem::file_type::character)
            j["type"] = "character";
        else if (ft == std::filesystem::file_type::fifo)
            j["type"] = "fifo";
        else if (ft == std::filesystem::file_type::socket)
            j["type"] = "socket";
        else
            j["type"] = "unknown";

        // For symlinks, stat the target to get size / entry_count
        auto target_st = is_symlink ? std::filesystem::status(*resolved, ec) : s;
        auto target_ft = target_st.type();
        if (target_ft == std::filesystem::file_type::regular) {
            uintmax_t sz = std::filesystem::file_size(*resolved, ec);
            if (!ec) j["size"] = sz;
        } else if (target_ft == std::filesystem::file_type::directory) {
            // Count entries for directories
            uintmax_t count = 0;
            auto it = std::filesystem::directory_iterator(
                *resolved,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            if (!ec) {
                for (auto& entry : it) {
                    ++count;
                }
                j["entry_count"] = count;
            }
        }

        // Permissions
        auto prms = s.permissions();
        auto has = [prms](std::filesystem::perms p) { return (prms & p) != std::filesystem::perms::none; };
        std::string perm_str;
        perm_str += has(std::filesystem::perms::owner_read)  ? 'r' : '-';
        perm_str += has(std::filesystem::perms::owner_write) ? 'w' : '-';
        perm_str += has(std::filesystem::perms::owner_exec)  ? 'x' : '-';
        perm_str += has(std::filesystem::perms::group_read)  ? 'r' : '-';
        perm_str += has(std::filesystem::perms::group_write) ? 'w' : '-';
        perm_str += has(std::filesystem::perms::group_exec)  ? 'x' : '-';
        perm_str += has(std::filesystem::perms::others_read)  ? 'r' : '-';
        perm_str += has(std::filesystem::perms::others_write) ? 'w' : '-';
        perm_str += has(std::filesystem::perms::others_exec)  ? 'x' : '-';
        j["permissions"] = perm_str;

        // Timestamps — convert to ISO 8601 strings
        auto to_iso = [](const std::filesystem::file_time_type& tp) -> std::string {
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                tp - std::filesystem::file_time_type::clock::now() +
                std::chrono::system_clock::now());
            std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
            std::tm tm{};
            gmtime_r(&tt, &tm);
            char buf[32];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            return buf;
        };

        auto mtime = std::filesystem::last_write_time(*resolved, ec);
        if (!ec) j["modified"] = to_iso(mtime);

        // created / birth time (not directly available in C++17 std::filesystem)
        // We skip it; the OS-dependent APIs for creation time are too varied.

        return j.dump();
    };
    return t;
}

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

Tool make_write_file_lines_tool(std::shared_ptr<std::string> safe_dir_ptr,
    FileModifiedCallback on_file_modified) {
    Tool t;
    t.name = "write_file_lines";
    t.description =
        "Replace a range of lines (start_line to end_line, 1-indexed) "
        "in a file with the given replacement text. "
        "Use this for targeted line-range edits instead of rewriting the entire file.";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "Path to the file to edit"}}},
                {"start_line",
                    {{"type", "integer"},
                        {"description",
                            "First line to replace (1-indexed)"}}},
                {"end_line",
                    {{"type", "integer"},
                        {"description",
                            "Last line to replace (inclusive)"}}},
                {"replace",
                    {{"type", "string"},
                        {"description",
                            "Replacement text to insert in place of the selected lines"}}}}},
        {"required", {"path", "start_line", "end_line", "replace"}}};
    t.execute = [safe_dir_ptr, on_file_modified](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());
        int start_line = args.value("start_line", 0);
        int end_line = args.value("end_line", 0);
        auto replace = args.value("replace", std::string());

        // Validate parameters
        if (start_line < 1) {
            return std::unexpected("start_line must be >= 1");
        }
        if (end_line < start_line) {
            return std::unexpected("end_line must be >= start_line");
        }

        // Resolve path
        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        // Check it's a regular file
        std::error_code ec;
        if (!std::filesystem::exists(*resolved, ec)) {
            return std::unexpected("File not found: " + *resolved);
        }
        if (!std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        // Read the entire file as a string
        std::ifstream ifs(*resolved, std::ios::binary);
        if (!ifs.is_open()) {
            return std::unexpected("Failed to read file: " + *resolved);
        }
        std::string content(
            (std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        ifs.close();

        // Detect trailing newline: if the file is not empty and the last
        // character before EOF was '\n', the file ends with a newline.
        bool has_trailing_newline = (!content.empty() && content.back() == '\n');

        // Split into lines using getline (which strips newline characters).
        std::vector<std::string> lines;
        {
            std::istringstream stream(content);
            std::string l;
            while (std::getline(stream, l)) {
                // Strip carriage return for Windows-style line endings
                if (!l.empty() && l.back() == '\r') {
                    l.pop_back();
                }
                lines.push_back(std::move(l));
            }
        }

        size_t num_lines = lines.size();

        // Clamp end_line to the number of lines
        if (end_line > (int)num_lines) {
            end_line = (int)num_lines;
        }

        // Validate: if start_line is beyond the last line, append at end
        std::string result;
        if (start_line > (int)num_lines) {
            // File has no lines at start_line position; append replace at end
            result = content;
            if (has_trailing_newline) {
                result += replace;
            } else {
                if (!result.empty() && result.back() != '\n') {
                    result += '\n';
                }
                result += replace;
            }
        } else {
            // Build the result from three parts: before, replacement, after
            std::string before;
            std::string after;

            // Lines before start_line (0 to start_line-2)
            for (int i = 0; i < start_line - 1; i++) {
                before += lines[i];
                before += '\n';
            }

            // Lines after end_line (end_line to num_lines-1)
            for (size_t i = (size_t)end_line; i < num_lines; i++) {
                after += lines[i];
                if (i < num_lines - 1 || has_trailing_newline) {
                    after += '\n';
                }
            }

            result = before + replace;
            if (!after.empty()) {
                // If replace doesn't end with newline and after doesn't start with newline,
                // we need to insert one to keep line separation
                if (!result.empty() && result.back() != '\n' && after.front() != '\n') {
                    result += '\n';
                }
                result += after;
            } else if (has_trailing_newline && (start_line <= (int)num_lines)) {
                // If we removed lines at the end but the original had trailing newline,
                // check if the replace text already has a trailing newline
                if (result.empty() || result.back() != '\n') {
                    result += '\n';
                }
            }
        }

        // Write the modified content back
        std::ofstream ofs(*resolved, std::ios::binary);
        if (!ofs.is_open()) {
            return std::unexpected("Failed to write file: " + *resolved);
        }
        ofs.write(result.data(), result.size());
        ofs.close();

        // Notify the callback
        if (on_file_modified) {
            on_file_modified(*resolved);
        }

        return "ok (replaced lines " + std::to_string(start_line) + "-" +
            std::to_string(end_line) + ", " +
            std::to_string(content.size()) + " bytes -> " +
            std::to_string(result.size()) + " bytes)";
    };
    return t;
}

Tool make_delete_file_tool(std::shared_ptr<std::string> safe_dir_ptr) {
    Tool t;
    t.name = "delete_file";
    t.description = "Delete a file";
    t.parameters = {{"type", "object"},
        {"properties",
            {{"path", {{"type", "string"}, {"description", "File path to delete"}}}}},
        {"required", {"path"}}};
    t.execute = [safe_dir_ptr](const json& args) -> Result<std::string> {
        auto raw = args.value("path", std::string());

        auto resolved = resolve_path(raw, *safe_dir_ptr);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (!std::filesystem::exists(*resolved, ec)) {
            return std::unexpected("File not found: " + *resolved);
        }
        if (!std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }
        uintmax_t size = std::filesystem::file_size(*resolved, ec);
        bool removed = std::filesystem::remove(*resolved, ec);
        if (ec || !removed) {
            return std::unexpected("Failed to delete file: " + ec.message());
        }
        return "ok (deleted " + *resolved + ", " + std::to_string(size) + " bytes)";
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
