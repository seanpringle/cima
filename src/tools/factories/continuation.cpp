#include "tools.h"

Tool make_schedule_continuation_tool(ContinuationSlot& slot, CancellationToken cancelled) {
    Tool t;
    t.name = "schedule_continuation";
    t.description =
        "Schedule a continuation of the current task. The provided `prompt` will be "
        "treated as a new user message after the current response completes. Use this "
        "to break long tasks into manageable turns, perform context compaction, or "
        "continue working after summarizing the conversation history.\n\n"
        "Guard rails:\n"
        "  - Max continuation steps per request: configurable via max_continuation_steps in cima.json "
        "(default 10, 0 = disabled)\n"
        "  - A short delay (default 250ms) is enforced between continuations to prevent "
        "accidental rapid-fire requests\n"
        "  - User cancellation is checked before scheduling and before processing";
    t.permission = ToolPermission::Internal;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"prompt",
                {{"type", "string"},
                    {"description",
                        "The user message to inject for the next turn. "
                        "This should concisely describe what to continue working on."}}}}},
        {"required", {"prompt"}}};
    t.execute = [&slot, cancelled](const json& args) -> Result<std::string> {
        if (*cancelled) {
            return std::unexpected(std::string("Task was cancelled"));
        }
        auto prompt = args.value("prompt", std::string());
        if (prompt.empty()) {
            return std::unexpected(std::string("prompt is required"));
        }
        if (slot.max_steps > 0 && slot.step_count >= slot.max_steps) {
            return std::unexpected("Maximum continuation steps (" + std::to_string(slot.max_steps) +
                ") reached");
        }
        slot.prompt = prompt;
        return "Continuation scheduled for the next turn.";
    };
    return t;
}
