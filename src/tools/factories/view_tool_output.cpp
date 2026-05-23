#include "tools.h"

#include <string>
#include <vector>

Tool make_view_tool_output_tool(
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "view_tool_output";
    
    // NOTE: view_tool_output does NOT append TOOL_LOG_NOTE — it is itself
    // the viewer for tool-log output, so its results are never redirected.
    t.description =
        "View the output of a previously executed tool that produced a large result."
        " Use the `id` from the long-output reference message."
        " Default action is to `tail -n100` the message."
        " Retrieve a range with `start_line` and `end_line`."
        " Lines are prefixed with line numbers.";

    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 30;

    t.parameters = {{"type", "object"},
        {"properties",
            {{"id",
                {{"type", "integer"},
                    {"description",
                        "`id` from the long-output reference message"}}},
            {"start_line",
                {{"type", "integer"},
                    {"description",
                        "First line to return (1-indexed within the stored log)."}}},
            {"end_line",
                {{"type", "integer"},
                    {"description",
                        "Last line to return (inclusive, 1-indexed)."}}},
            }},
        {"required", {"id"}}};

    t.execute = [tool_logs](const json& args) -> Result<std::string> {

        for (auto& el: args.items()) {
            if (el.key() == "id") continue;
            if (el.key() == "start_line") continue;
            if (el.key() == "end_line") continue;
            return std::unexpected("unknown argument: " + el.key());
        }

        int id = args.value("id", 0);
        if (id < 1) {
            return std::unexpected("id " + std::to_string(id) + " is invalid");
        }

        size_t idx = static_cast<size_t>(id) - 1;

        if (!tool_logs || idx >= tool_logs->size()) {
            return std::unexpected("id " + std::to_string(id) + " is missing or invalid");
        }

        const std::string& content = (*tool_logs)[idx];

        int start_line = args.value("start_line", 1);

        if (args.contains("start_line") && start_line < 1) {
            return std::unexpected("start_line must be >= 1");
        }

        int end_line = args.value("end_line", start_line+99);

        if (end_line < start_line) {
            return std::unexpected("end_line must be >= start_line");
        }

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

        if (end_line > int(lines.size())) {
            end_line = int(lines.size());
        }

        std::stringstream ss;

        ss << "Tool output id " << id
            << " lines " << start_line << ':' << end_line
            << " of " << lines.size()
            << '\n';

        for (int i = start_line; i <= int(lines.size()) && i <= end_line; i++) {
            ss << i << ": " << lines[i-1] << '\n';
        }

        return ss.str();
    };
    return t;
}
