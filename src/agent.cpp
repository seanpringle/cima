
#include "agent.h"
#include "client.h"
#include "config.h"

#include <algorithm>
#include <expected>
#include <optional>
#include <thread>

void Agent::cancel_and_wait() {
    if (chat_state->running) {
        *chat_state->cancelled = true;
        if (chat_state->future.valid()) {
            chat_state->future.wait();
            try {
                chat_state->future.get();
            } catch (...) {
            }
        }
        chat_state->running = false;
    }
}

std::optional<Result<ChatResult>> Agent::check_finished() {
    if (chat_state->running && chat_state->future.valid() &&
        chat_state->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        chat_state->running = false;
        try {
            return chat_state->future.get();
        } catch (const std::exception& e) {
            return std::unexpected(std::string(e.what()));
        }
    }
    return std::nullopt;
}

/// Signal cancellation on all running chats (called on quit).
void PrimaryAgent::cancel_running_chats() {
    if (chat_state->running)
        *chat_state->cancelled = true;
    for (auto& t : subagents) {
        if (t.chat_state->running)
            *t.chat_state->cancelled = true;
    }
}

PrimaryAgent::PrimaryAgent(Session& session) : session_{session} {
    init_defaults();
    create_chat_session();
    restore_session_data();
    create_subagents();
    register_subagent_tools();
}

PrimaryAgent::~PrimaryAgent() {
    for (auto& t : subagents)
        t.cancel_and_wait();
    cancel_and_wait();

    // Stop all custom MCP servers before saving session data.
    for (const auto& mcp : session_.session_data().custom_mcp_servers) {
        if (session->mcp_registry().is_running(mcp.name)) {
            session->stop_custom_mcp_server(mcp.name);
        }
    }

    session_.session_data().provider_name = provider_name;
    session_.session_data().model = model_name;
    session_.session_data().reasoning_effort = reasoning_effort;
    session_.session_data().conversation = session->conversation().to_json();

    json log_arr = json::array();
    for (const auto& e : ui_state.entries) {
        json entry;
        switch (e.type) {
        case EntryType::UserText:
            entry["type"] = "UserText";
            break;
        case EntryType::Reasoning:
            entry["type"] = "Reasoning";
            break;
        case EntryType::Content:
            entry["type"] = "Content";
            break;
        case EntryType::ToolCall:
            entry["type"] = "ToolCall";
            break;
        }
        entry["text"] = e.text;
        if (!e.tool_result.empty())
            entry["tool_result"] = e.tool_result;
        if (e.is_streaming)
            entry["streaming"] = true;
        log_arr.push_back(std::move(entry));
    }
    session_.session_data().chat_log = std::move(log_arr);
    session_.session_data().plan = session->plan().to_json();
    session_.session_data().mcp_enabled = mcp_enabled;
    session_.session_data().tool_gates = tool_gates;
    session_.session_data().rw_subagent_tool_gates = rw_subagent_tool_gates;
    session_.session_data().ro_subagent_tool_gates = ro_subagent_tool_gates;
    session_.session_data().input_history.assign(
        ui_state.input_history.begin(), ui_state.input_history.end());

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec)
        session_.session_data().last_cwd = cwd.string();
}

void PrimaryAgent::init_defaults() {
    id = 1;
    title = "Assistant";

    if (cfg.providers.empty())
        throw std::runtime_error("No providers configured");

    const auto& provider = cfg.providers[0];
    provider_name = provider.name;
    model_name = provider.model;
    reasoning_effort = provider.reasoning_effort;
}

void PrimaryAgent::create_chat_session() {
    if (cfg.providers.empty())
        throw std::runtime_error("No providers configured");
    const auto& provider = cfg.providers[0];

    chat_state = std::make_unique<AsyncChatState>();
    session = std::make_unique<ChatSession>(cfg, provider, chat_state->cancelled);
    session->set_agent_name("Assistant");
    session->set_output_callback([cs = chat_state.get()](const std::string& text, OutputType type) {
        std::lock_guard<std::mutex> lock(cs->mutex);
        cs->pending.emplace_back(text, type);
    });
}

void PrimaryAgent::restore_session_data() {
    auto& sd = session_.session_data();
    if (!sd.provider_name.empty()) {
        provider_name = sd.provider_name;
        for (const auto& p : cfg.providers) {
            if (p.name == sd.provider_name) {
                session->set_provider(p);
                break;
            }
        }
    }

    if (!sd.model.empty()) {
        model_name = sd.model;
        session->set_model(sd.model);
    }

    if (!sd.reasoning_effort.empty()) {
        reasoning_effort = sd.reasoning_effort;
        session->set_reasoning_effort(sd.reasoning_effort);
    }

    session->conversation().from_json(sd.conversation);

    ui_state.entries.clear();
    if (sd.chat_log.is_array()) {
        for (const auto& entry : sd.chat_log) {
            DisplayEntry e;
            std::string t = entry.value("type", "Content");
            if (t == "UserText")
                e.type = EntryType::UserText;
            else if (t == "Reasoning")
                e.type = EntryType::Reasoning;
            else if (t == "Content")
                e.type = EntryType::Content;
            else if (t == "ToolCall")
                e.type = EntryType::ToolCall;
            else
                continue;
            e.text = entry.value("text", "");
            e.tool_result = entry.value("tool_result", "");
            e.is_streaming = entry.value("streaming", false);
            ui_state.entries.push_back(std::move(e));
        }
    }

    if (sd.plan.is_object()) {
        session->plan().from_json(sd.plan);
    }

    mcp_enabled = sd.mcp_enabled;

    // ── Tool gates: restore from session data ──
    tool_gates.clear();
    // Override with persisted values from session data.
    for (const auto& [name, enabled] : sd.tool_gates) {
        tool_gates[name] = enabled;
    }
    // Propagate to session.
    for (const auto& [name, enabled] : tool_gates) {
        session->set_tool_enabled(name, enabled);
    }

    // ── Read-write subagent gates: same defaults as primary ──
    // All tools enabled by default, except call_subagent
    // (subagents must not recurse into other subagents).
    rw_subagent_tool_gates.clear();
    rw_subagent_tool_gates["call_subagent"] = false;
    // Override with persisted values.
    for (const auto& [name, enabled] : sd.rw_subagent_tool_gates) {
        rw_subagent_tool_gates[name] = enabled;
    }

    // ── Read-only subagent gates: only read-only tools start ON ──
    ro_subagent_tool_gates.clear();
    ro_subagent_tool_gates["read_file"] = true;
    ro_subagent_tool_gates["read_file_lines"] = true;
    ro_subagent_tool_gates["grep_files"] = true;
    ro_subagent_tool_gates["web_search"] = true;
    ro_subagent_tool_gates["web_fetch"] = true;
    // All write tools, bash, call_subagent default to false (missing = false)
    // Override with persisted values.
    for (const auto& [name, enabled] : sd.ro_subagent_tool_gates) {
        ro_subagent_tool_gates[name] = enabled;
    }

    // Restore custom MCP servers: start any that were previously enabled.
    for (const auto& mcp : sd.custom_mcp_servers) {
        if (mcp_enabled.count(mcp.name) && mcp_enabled[mcp.name]) {
            auto result = session->start_custom_mcp_server(mcp);
            if (!result)
                mcp_error[mcp.name] = result.error();
        }
    }

    // Restore input history
    ui_state.input_history.clear();
    for (const auto& item : sd.input_history) {
        ui_state.input_history.push_back(item);
    }

    // Apply session knob overrides (if any)
    session_.apply_knobs_to(*session);
}

void PrimaryAgent::create_subagents() {
    int next_id = 2;

    for (const auto& sa : cfg.subagents) {
        if (cfg.providers.empty())
            throw std::runtime_error("No providers configured");
        const auto& provider = cfg.providers[0];

        auto& sub_agent = subagents.emplace_back();
        sub_agent.id = next_id++;
        sub_agent.subagent_name = sa.name;
        sub_agent.title = sa.name;
        sub_agent.read_only_tools = sa.read_only;
        sub_agent.provider_name = provider.name;
        sub_agent.model_name = provider.model;
        sub_agent.reasoning_effort = provider.reasoning_effort;

        sub_agent.chat_state = std::make_unique<AsyncChatState>();
        // Each subagent gets its own GatingState (independent from primary)
        // so gate toggles in the Tool Calls table affect each mode separately.
        sub_agent.session = ChatSession::create_subagent(
            cfg, provider, sa.read_only, sub_agent.chat_state->cancelled, nullptr);
        sub_agent.session->set_agent_name(sa.name);
        sub_agent.session->set_output_callback(
            [cs = sub_agent.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });

        // Apply the appropriate gate map to this subagent.
        const auto& gates = sa.read_only ? ro_subagent_tool_gates : rw_subagent_tool_gates;
        for (const auto& [name, enabled] : gates) {
            sub_agent.session->set_tool_enabled(name, enabled);
        }
    }
}

Result<SubAgent*> PrimaryAgent::subagent_by_name(const std::string& name) {
    for (auto& t : subagents)
        if (t.subagent_name == name)
            return &t;
    return std::unexpected("no such subagent: " + name);
}

void PrimaryAgent::register_subagent_tools() {
    session->register_call_subagent_tool(
        *this, cfg.subagents); // uses cfg.subagent_timeout internally
}

// ── ChatUIState methods ─────────────────────────────────────────────────────

void ChatUIState::push_entry(EntryType type, const std::string& text, bool streaming) {
    DisplayEntry entry{type, text, streaming};
    entries.push_back(entry);
}

void ChatUIState::finalize_streaming_entry() {
    if (!entries.empty() && entries.back().is_streaming) {
        entries.back().is_streaming = false;
    }
}

// ── Agent methods ──────────────────────────────────────────────────────────

void Agent::start_chat(std::string input) {
    chat_state->running = true;
    *chat_state->cancelled = false;
    chat_state->future = std::async(std::launch::async,
        [this, input = std::move(input)]() { return session->run_once(input); });
}

void Agent::drain_pending() {
    auto& ui = ui_state;
    std::lock_guard<std::mutex> lock(chat_state->mutex);
    for (auto& [pending_text, type] : chat_state->pending) {
        if (type == OutputType::ToolInvocation) {
            ui.finalize_streaming_entry();
            ui.push_entry(EntryType::ToolCall, pending_text, false);
        } else if (type == OutputType::ToolResult) {
            // Attach result to the most recent ToolCall entry without a result
            for (auto it = ui.entries.rbegin(); it != ui.entries.rend(); ++it) {
                if (it->type == EntryType::ToolCall && it->tool_result.empty()) {
                    it->tool_result = pending_text;
                    break;
                }
            }
        } else {
            auto entry_type =
                (type == OutputType::Reasoning) ? EntryType::Reasoning : EntryType::Content;
            if (!ui.entries.empty() && ui.entries.back().is_streaming &&
                ui.entries.back().type == entry_type) {
                ui.entries.back().text += pending_text;
            } else {
                ui.finalize_streaming_entry();
                ui.push_entry(entry_type, pending_text, true);
            }
        }
    }
    chat_state->pending.clear();
}

void Agent::validate_current_model() {
    auto& ui = ui_state;
    auto& entry = g_provider_models[provider_name];
    if (entry.fetched && !ui.models_validated && !entry.models.empty()) {
        ui.models_validated = true;
        const auto& current = session->model();
        bool found = std::any_of(entry.models.begin(),
            entry.models.end(),
            [&current](const std::string& m) { return m == current; });
        if (!found) {
            session->set_model(entry.models.front());
            model_name = entry.models.front();
        }
    }
}

std::optional<Result<void>> Agent::poll_compact() {
    auto& ui = ui_state;

    // Launch compaction if requested and not already running
    if (ui.compact_requested && !chat_state->compact_running) {
        ui.compact_requested = false;
        chat_state->compact_running = true;
        chat_state->compact_future = std::async(std::launch::async, [this]() {
            return session->compact();
        });
    }

    // Poll for completion if compact is in-flight
    if (chat_state->compact_running && chat_state->compact_future.valid() &&
        chat_state->compact_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        chat_state->compact_running = false;
        try {
            auto result = chat_state->compact_future.get();
            if (result) {
                ui.push_entry(EntryType::Content, "Conversation compacted.", false);
            } else {
                ui.push_entry(EntryType::Content, "Compaction failed: " + result.error(), false);
            }
            return std::move(result);
        } catch (const std::exception& e) {
            ui.push_entry(EntryType::Content, "Compaction failed: " + std::string(e.what()), false);
            return std::unexpected(std::string(e.what()));
        }
    }

    return std::nullopt;
}

void Agent::poll_clear() {
    auto& ui = ui_state;
    if (ui.clear_requested) {
        ui.clear_requested = false;
        session->clear();
        ui.entries.clear();
        ui.push_entry(EntryType::Content, "Conversation cleared.", false);
    }
}
