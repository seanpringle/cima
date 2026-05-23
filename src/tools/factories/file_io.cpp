#include "tools.h"

#include <git2.h>

#include <filesystem>
#include <fstream>
#include <string>

Tool make_read_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    const std::vector<std::string>& read_only_paths,
    std::shared_ptr<std::vector<std::string>> tool_logs) {

    Tool t;
    t.name = "read_file";

    t.description = "Read a block of lines from a file."
        " Start with any small range; the discovered total line count will be reported."
        " Lines are prefixed with line numbers, 1-based."
    ;

    t.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Path to the file to read."}}},
            {"start_line", {{"type", "integer"}, {"description", "Beginning of range, 1-based"}}},
            {"end_line", {{"type", "integer"}, {"description", "End of range (inclusive): 1-based"}}},
        }},
        {"required", {"path","start_line","end_line"}}
    };

    t.execute = [safe_dir_ptr, read_only_paths, tool_logs](const json& args) -> Result<std::string> {

        for (auto& el: args.items()) {
            if (el.key() == "path") continue;
            if (el.key() == "start_line") continue;
            if (el.key() == "end_line") continue;
            return std::unexpected("unknown argument: " + el.key());
        }

        auto raw = args.value("path", std::string());
        auto resolved = resolve_path(raw, *safe_dir_ptr, read_only_paths);
        if (!resolved) {
            return std::unexpected(resolved.error());
        }

        std::error_code ec;
        if (std::filesystem::exists(*resolved, ec) &&
            !std::filesystem::is_regular_file(*resolved, ec)) {
            return std::unexpected("Not a regular file: " + *resolved);
        }

        std::ifstream file(*resolved);
        if (!file.is_open()) {
            return std::unexpected("Failed to open file: " + *resolved);
        }

        std::string result(std::istreambuf_iterator<char>{file}, {});

        int start_line = args.value("start_line",1);
        int end_line = args.value("end_line",1);

        std::stringstream ss;
        ss << "read_file " << raw << '\n' << format_line_range(result, start_line, end_line);
        return ss.str();
    };
    return t;
}

Tool make_write_file_tool(std::shared_ptr<std::string> safe_dir_ptr,
    FileModifiedCallback on_file_modified) {
    Tool t;
    t.name = "write_file";
    t.description = "Write content to a file, creating parent directories if needed";

    t.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "File path"}}},
            {"content", {{"type", "string"}, {"description", "Content to write"}}}
        }},
        {"required", {"path", "content"}}
    };

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

    t.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {
                {"type", "string"}, {"description", "File path to edit"}}},
            {"search", {
                {"type", "string"},
                {"description",
                    "Exact string to search for; must match exactly once in the file. "
                    "Include surrounding context (unique nearby lines) to guarantee a "
                    "single match."}}},
            {"replace", {
                {"type", "string"},
                {"description", "String to replace the matched occurrence with"}}}
        }},
        {"required", {"path", "search", "replace"}}
    };

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

        // Save old content for diff generation before modifying
        std::string old_content = content;

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

        // Generate a unified diff of the change using libgit2
        std::string diff_text;
        {
            git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
            auto print_cb = [](const git_diff_delta* /*delta*/,
                               const git_diff_hunk* /*hunk*/,
                               const git_diff_line* line,
                               void* payload) -> int {
                auto* output = static_cast<std::string*>(payload);
                // Prepend origin character for +/-/context lines
                if (line->origin == '+' || line->origin == '-' || line->origin == ' ') {
                    output->push_back(line->origin);
                }
                output->append(line->content, line->content_len);
                return 0;
            };
            int err = git_diff_buffers(
                old_content.data(), old_content.size(), resolved->c_str(),
                content.data(), content.size(), resolved->c_str(),
                &diff_opts,
                nullptr, nullptr, nullptr,
                print_cb, &diff_text);
            if (err) {
                diff_text.clear(); // fall back to no diff
            }
        }

        std::string result = "ok (replaced 1 occurrence at line " + std::to_string(line_num) +
            ", " + std::to_string(search.size()) + " bytes -> " +
            std::to_string(replace.size()) + " bytes)";
        if (!diff_text.empty()) {
            result += " diff follows:\n" + diff_text;
        }
        return result;
    };
    return t;
}


