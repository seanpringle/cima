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
        return "(long tool output: " + std::to_string(nl) + " lines, " +
               std::to_string(tool_logs->back().size()) + " chars. "
               "Use view_tool_output(id=" + std::to_string(id) + ") to read it)";
    }

    return output;
}
