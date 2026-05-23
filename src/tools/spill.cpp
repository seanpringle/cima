#include "tools.h"

// ===================================================================
// spill_long_output — move oversized tool output into the tool_logs
// buffer and return a short reference string instead.
// ===================================================================

std::string spill_long_output(std::string output,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    if (!tool_logs) {
        return output;
    }

    int nl = 0;
    for (char c : output)
        if (c == '\n') nl++;

    if (nl > 100 || output.size() > 4096) {
        size_t id = tool_logs->size() + 1;
        tool_logs->push_back(std::move(output));
        return "\u26a0 Tool log (" + std::to_string(nl) + " lines, " +
               std::to_string(tool_logs->back().size()) + " chars): "
               "call view_tool_output(id=" + std::to_string(id) + ") to see full output.";
    }

    return output;
}

std::string format_line_range(const std::string& content, int start_line, int end_line) {
    std::vector<std::string> lines = {{}};

    for (auto c: content) {
        if (c == '\n') {
            lines.emplace_back();
            continue;
        }
        lines.back().push_back(c);
    }

    // fix loop adding extra line
    if (lines.size() > 1 && content.back() == '\n') {
        lines.pop_back();
    }

    start_line = std::max(1, std::min(start_line, int(lines.size())));
    end_line = std::max(start_line, std::min(end_line, int(lines.size())));

    std::stringstream ss;

    ss << "lines " << start_line << ':' << end_line
        << " of " << lines.size()
        << '\n';

    for (int i = start_line; i <= int(lines.size()) && i <= end_line; i++) {
        ss << i << ": " << lines[i-1] << '\n';
    }

    return ss.str();
}
