
#include "agent.h"
#include "session_data.h"
#include <expected>

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
    if (ui_state.models_future.valid()) {
        ui_state.models_future.wait();
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

PrimaryAgent::PrimaryAgent(SessionData& data) : session_data{data} {
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

    session_data.provider_name = provider_name;
    session_data.model = model_name;
    session_data.reasoning_effort = reasoning_effort;
    session_data.conversation = session->conversation().to_json();

    json log_arr = json::array();
    for (const auto& e : ui_state.entries) {
        json entry;
        entry["seq"] = e.seq;
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
        if (e.is_streaming)
            entry["streaming"] = true;
        log_arr.push_back(std::move(entry));
    }
    session_data.chat_log = std::move(log_arr);
    session_data.plan = session->plan().to_json();
    session_data.bash_enabled = bash_enabled;
    session_data.mcp_enabled = mcp_enabled;

    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (!ec)
        session_data.last_cwd = cwd.string();
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
    if (!session_data.provider_name.empty()) {
        provider_name = session_data.provider_name;
        for (const auto& p : cfg.providers) {
            if (p.name == session_data.provider_name) {
                session->set_provider(p);
                break;
            }
        }
    }

    if (!session_data.model.empty()) {
        model_name = session_data.model;
        session->set_model(session_data.model);
    }

    if (!session_data.reasoning_effort.empty()) {
        reasoning_effort = session_data.reasoning_effort;
        session->set_reasoning_effort(session_data.reasoning_effort);
    }

    session->conversation().from_json(session_data.conversation);

    ui_state.entries.clear();
    int max_seq = 0;
    if (session_data.chat_log.is_array()) {
        for (const auto& entry : session_data.chat_log) {
            DisplayEntry e;
            e.seq = entry.value("seq", 0);
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
            e.is_streaming = entry.value("streaming", false);
            ui_state.entries.push_back(std::move(e));
            if (e.seq > max_seq)
                max_seq = e.seq;
        }
    }
    ui_state.next_seq = max_seq + 1;

    if (session_data.plan.is_object()) {
        session->plan().from_json(session_data.plan);
    }

    bash_enabled = session_data.bash_enabled;
    session->set_bash_enabled(session_data.bash_enabled);
    mcp_enabled = session_data.mcp_enabled;
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
        sub_agent.session = ChatSession::create_subagent(
            cfg, provider, sa.read_only, sub_agent.chat_state->cancelled);
        sub_agent.session->set_agent_name(sa.name);
        sub_agent.session->set_output_callback(
            [cs = sub_agent.chat_state.get()](const std::string& text, OutputType type) {
                std::lock_guard<std::mutex> lock(cs->mutex);
                cs->pending.emplace_back(text, type);
            });
    }
}

Result<SubAgent*> PrimaryAgent::subagent_by_name(const std::string& name) {
    for (auto& t : subagents)
        if (t.subagent_name == name)
            return &t;
    return std::unexpected("no such subagent: " + name);
}

void PrimaryAgent::register_subagent_tools() {
    session->register_call_subagent_tool(*this, cfg.subagents);
}
