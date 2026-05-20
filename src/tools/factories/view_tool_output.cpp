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
        "IDs are 1-based. Returns lines prefixed with line numbers, like read_file_lines.\n"
        "If H>0, shows the first H lines. If T>0, shows the last T lines (of the "
        "head-filtered set if both are specified). Overrides start_line/end_line/max_lines.";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 30;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"id",
                {{"type", "integer"},
                    {"description",
                        "1-based ID from the long-output reference message"}}},
             {"head",
                {{"type", "integer"},
                    {"description", "Take first N lines (0 = no head filter, default 0)"}}},
             {"tail",
                {{"type", "integer"},
                    {"description", "Take last N lines (0 = no tail filter, default 0)"}}},
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

        int head = args.value("head", 0);
        int tail = args.value("tail", 0);
        int start_line = args.value("start_line", 1);
        int end_line = args.value("end_line", 0); // 0 = not specified
        int max_lines = args.value("max_lines", 200);

        if (start_line < 1)
            return std::unexpected("start_line must be >= 1");
        if (end_line != 0 && end_line < start_line)
            return std::unexpected("end_line must be >= start_line");
        if (max_lines < 1) max_lines = 1;
        if (max_lines > 500) max_lines = 500;

        // Split content into lines, tracking original line numbers
        std::vector<std::pair<int, std::string>> all_lines;
        size_t pos = 0;
        int line_num = 0;
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
            all_lines.emplace_back(line_num, std::move(line));
        }
        int total_lines = line_num;

        // Determine which lines to show based on read_file_lines-style params
        int range_start = start_line;
        int range_end = end_line != 0 ? end_line : total_lines;
        if (range_end > total_lines) range_end = total_lines;

        int max_to_read = max_lines;
        if (end_line != 0) {
            int range = end_line - start_line + 1;
            if (range < max_to_read) max_to_read = range;
        }

        // Collect lines in the range
        std::vector<std::pair<int, std::string>> result_lines;
        for (const auto& [ln, text] : all_lines) {
            if (ln >= range_start && ln <= range_end &&
                static_cast<int>(result_lines.size()) < max_to_read) {
                result_lines.emplace_back(ln, text);
            }
        }
        bool line_range_exhausted = (static_cast<int>(result_lines.size()) < max_to_read &&
            (end_line == 0 || range_end >= total_lines));

        // Apply head/tail (overrides read_file_lines filtering if specified)
        if (head > 0 && static_cast<int>(result_lines.size()) > head) {
            result_lines.resize(head);
        }
        if (tail > 0 && static_cast<int>(result_lines.size()) > tail) {
            result_lines.erase(result_lines.begin(),
                result_lines.begin() + (result_lines.size() - tail));
        }

        // Format output
        std::string result;
        for (const auto& [ln, text] : result_lines) {
            result += std::to_string(ln) + ": " + text + "\n";
        }
        int shown = static_cast<int>(result_lines.size());

        // Determine if there's more content beyond what was shown
        bool has_more = false;
        int remaining = 0;
        if (head > 0 || tail > 0) {
            // With head/tail, just report if there are hidden lines
            int hidden = total_lines - shown;
            if (hidden > 0) {
                has_more = true;
                remaining = hidden;
            }
        } else if (end_line != 0) {
            // Using end_line: check if we didn't reach end_line
            int last_shown = shown > 0 ? result_lines.back().first : start_line - 1;
            if (last_shown < end_line) {
                has_more = true;
                remaining = (end_line - last_shown);
            }
        } else {
            // Using default end (to EOF): check if we hit the max_lines cap
            bool capped = (shown >= max_to_read && shown < total_lines);
            if (capped) {
                has_more = true;
                remaining = total_lines - (shown > 0 ? result_lines.back().first : 0);
            }
        }

        if (has_more) {
            result += "...(truncated, >" + std::to_string(shown + remaining) +
                " lines from line " + std::to_string(start_line) + ")";
        } else if (shown == 0 && start_line > 1) {
            result = "(start_line " + std::to_string(start_line) +
                " is beyond end of file)";
        }

        return result;
    };
    return t;
}
