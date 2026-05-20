#include "tools.h"

#include <string>
#include <vector>

Tool make_view_tool_output_tool(
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "view_tool_output";
    t.description =
        "View the output of a previously executed tool that produced a large result. "
        "Use the 'id' from the long-output reference message. "
        "IDs are 1-based. Returns lines prefixed with line numbers, like read_file_lines.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"id",
                {{"type", "integer"},
                    {"description",
                        "1-based ID from the long-output reference message"}}},
             {"start_line",
                {{"type", "integer"},
                    {"description",
                        "First line to return (1-indexed within the stored log, default 1)"}}},
             {"end_line",
                {{"type", "integer"},
                    {"description",
                        "Last line to return (inclusive). If omitted, reads to "
                        "end (capped by max_lines)."}}},
             {"max_lines",
                {{"type", "integer"},
                    {"description",
                        "Maximum lines to return (default 200, max 500)"}}}}},
        {"required", {"id"}}};
    t.execute = [tool_logs](const json& args) -> Result<std::string> {
        int id = args.value("id", 0);
        if (id < 1) {
            return std::unexpected("id must be >= 1 (IDs are 1-based)");
        }
        size_t idx = static_cast<size_t>(id) - 1;
        if (!tool_logs || idx >= tool_logs->size()) {
            return std::unexpected("id " + std::to_string(id) +
                " not found (there are " +
                std::to_string(tool_logs ? tool_logs->size() : 0) +
                " stored outputs)");
        }

        const std::string& content = (*tool_logs)[idx];

        int start_line = args.value("start_line", 1);
        int end_line = args.value("end_line", 0); // 0 = not specified
        int max_lines = args.value("max_lines", 200);

        if (start_line < 1)
            return std::unexpected("start_line must be >= 1");
        if (end_line != 0 && end_line < start_line)
            return std::unexpected("end_line must be >= start_line");
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        // Walk lines to find the range
        std::string result;
        int line_num = 0;
        int count = 0;

        // Determine max lines to read
        int max_to_read = max_lines;
        if (end_line != 0) {
            int range = end_line - start_line + 1;
            if (range < max_to_read) max_to_read = range;
        }

        // Scan through content line by line
        size_t pos = 0;
        while (pos < content.size()) {
            size_t next = content.find('\n', pos);
            std::string line;
            if (next == std::string::npos) {
                line = content.substr(pos);
                pos = content.size();
            } else {
                line = content.substr(pos, next - pos);
                pos = next + 1;
            }
            line_num++;

            if (line_num >= start_line && count < max_to_read) {
                result += std::to_string(line_num) + ": " + line + "\n";
                count++;
            }

            if (count >= max_to_read)
                break;
        }

        // Check if there are more lines
        bool has_more = (pos < content.size()) ||
                        (end_line != 0 && line_num < end_line);
        if (has_more) {
            // Count remaining lines
            int remaining = 0;
            while (pos < content.size()) {
                size_t next = content.find('\n', pos);
                if (next == std::string::npos) break;
                pos = next + 1;
                remaining++;
            }
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
