#include "tools.h"
#include "gui_app.h" // for AsyncChatState

// ===================================================================
// Tool: ask_user
// ===================================================================
//
// Presents a question to the user via a modal popup and waits for
// their response.  The background thread blocks on a future; the GUI
// thread fulfills the promise when the user interacts with the modal.
//
// Three input types:
//   "text"    – Free-form text input
//   "confirm" – Yes/No confirmation, plus an optional override field
//   "choice"  – Select from a list of options, plus an override field
//
// ===================================================================

Tool make_ask_user_tool(AsyncChatState& chat_state) {
    Tool t;
    t.name = "ask_user";
    t.description =
        "Present a question to the user and wait for their response. "
        "Use this when you need input, confirmation, or a decision from the user.\n\n"
        "Input types:\n"
        "  - \"text\":    Free-form text input (user types a response)\n"
        "  - \"confirm\": Yes/No confirmation (returns \"yes\" or \"no\")\n"
        "  - \"choice\":  Select from a list of options (returns the chosen option)\n"
        "\n"
        "Examples:\n"
        "  ask_user({\"question\": \"May I delete the build/ directory?\", \"input_type\": \"confirm\"})\n"
        "  ask_user({\"question\": \"What port should the server use?\", \"input_type\": \"text\", \"default_value\": \"8080\"})\n"
        "  ask_user({\"question\": \"Which test suite to run?\", \"input_type\": \"choice\", \"options\": [\"unit\", \"integration\", \"all\"]})\n";
    t.permission = ToolPermission::ReadOnly;
    t.timeout_sec = 0; // no timeout — user decides when to respond
    t.parameters = {{"type", "object"},
        {"properties",
            {{"question",
                 {{"type", "string"}, {"description", "The question to present to the user"}}},
                {"input_type",
                    {{"type", "string"},
                        {"enum", {"text", "confirm", "choice"}},
                        {"description", "Type of input expected"}}},
                {"options",
                    {{"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Choices for 'choice' input_type"}}},
                {"default_value",
                    {{"type", "string"},
                        {"description", "Default / pre-filled value for text input"}}}}},
        {"required", {"question"}}};

    t.execute = [&chat_state](const json& args) -> Result<std::string> {
        std::string question    = args.value("question", "");
        std::string input_type  = args.value("input_type", "text");
        auto options            = args.value("options", std::vector<std::string>{});
        std::string default_val = args.value("default_value", "");

        if (question.empty()) {
            return std::unexpected("ask_user: question must not be empty");
        }

        // Build the request (constructor creates the future from the promise)
        UserInputRequest req;
        req.question      = std::move(question);
        req.input_type    = std::move(input_type);
        req.options       = std::move(options);
        req.default_value = std::move(default_val);

        // Get the future before publishing (move would invalidate req.future)
        auto future = std::shared_future<std::string>(req.future.share());

        // Publish to the GUI via chat_state (thread-safe)
        {
            std::lock_guard<std::mutex> lock(chat_state.input_mutex);
            chat_state.pending_user_input = std::move(req);
        }

        // ── BLOCK here until the GUI fulfills the promise ──
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            if (*chat_state.cancelled) {
                return std::unexpected("ask_user cancelled");
            }
        }

        // Future is ready — get the response
        try {
            return future.get();
        } catch (const std::exception& e) {
            return std::unexpected(std::string("ask_user interrupted: ") + e.what());
        }
    };

    return t;
}
