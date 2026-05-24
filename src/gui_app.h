#pragma once

#include "chat.h"
#include "config.h"
#include "types.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// ── Shared provider model cache ────────────────────────────────────────
// Probed once at startup for every provider; all tabs read from this.
struct ProviderModels {
    bool fetched = false; // publication flag (release/acquire)
    std::vector<std::string> models;
    std::string error;
};

using ProviderModelCache = std::map<std::string, ProviderModels>;

/// The global provider model cache. Populated async in gui_main() before
/// the main loop, then read by config / subagent tabs.
extern ProviderModelCache g_provider_models;

struct AsyncChatState {
    std::mutex mutex;
    std::vector<std::pair<std::string, OutputType>> pending;
    std::atomic<bool> running{false};
    std::future<Result<ChatResult>> future;
    CancellationToken cancelled = make_cancellation_token();

    // Compaction state (runs in background thread)
    std::future<Result<void>> compact_future;
    bool compact_running = false;

    // ── ask_user bridge ──
    std::optional<UserInputRequest> pending_user_input;
    std::mutex input_mutex;
};

struct ImFont;

struct ChatUIState {
    std::vector<DisplayEntry> entries;
    bool request_cancel = false;
    /// Create a DisplayEntry with the given type/text/streaming flag, push to
    /// entries, and log non-streaming entries immediately.
    void push_entry(EntryType type, const std::string& text, bool streaming);

    /// Finalise the last streaming entry (mark non-streaming, log it).
    void finalize_streaming_entry();

    bool models_validated = false; // true once the render thread has applied auto-select

    // Compact button state (synchronous, no async needed)
    bool compact_requested = false;

    // Clear button state (synchronous, no async needed)
    bool clear_requested = false;

    std::deque<std::string> input_history;
    std::vector<char> input_buffer = {0};
    int cursor_pos = 0; // tracked by InputText callback for insert-at-cursor

    // ── ask_user ──
    std::optional<UserInputRequest> active_ask_user; // active modal request (GUI side)

    // (workspace_path_buf removed — safe_dir locked to cwd)
};

int gui_main(const std::string& session_name, ConfigPtr cfg, PlanBoardPtr plan);
