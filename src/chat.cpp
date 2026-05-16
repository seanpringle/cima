#include "chat.h"
#include "lsp/json_rpc.h"
#include "plan.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

ChatSession::ChatSession(const Config& config, const Provider& provider,
    CancellationToken cancelled)
    : model_(provider.model), reasoning_effort_(provider.reasoning_effort),
      provider_name_(provider.name),
      safe_dir_(std::make_shared<std::string>(
          std::filesystem::current_path().string())),
      api_base_(provider.api_base), api_key_(provider.api_key),
      max_iterations_(config.max_tool_iterations),
      context_limit_(provider.context_limit),
      system_prompt_(config.system_prompt),
      client_(provider.api_base, provider.api_key),
      file_modified_cb_(std::make_shared<FileModifiedCallback>()),
      cancelled_(cancelled ? std::move(cancelled) : make_cancellation_token()) {
    // Share the cancellation token with tools and client
    tools_.set_cancelled(cancelled_);
    client_.set_cancelled(cancelled_);

    tools_.add_defaults(safe_dir_, config, /*include_write=*/true, *file_modified_cb_);

    // Each session gets its own plan tools tied to its PlanBoard
    tools_.add(make_write_plan_tool(plan_));
    tools_.add(make_read_plan_tool(plan_));

    // Each session gets its own notes tools tied to its Notes storage.
    tools_.add(make_list_notes_tool(notes_));
    tools_.add(make_read_note_tool(notes_));
    tools_.add(make_write_note_tool(notes_));
    tools_.add(make_delete_note_tool(notes_));

}

void ChatSession::set_provider(const Provider& provider) {
    provider_name_ = provider.name;
    api_base_ = provider.api_base;
    api_key_ = provider.api_key;
    model_ = provider.model;
    reasoning_effort_ = provider.reasoning_effort;
    context_limit_ = provider.context_limit;
    context_limit_discovered_ = false; // will re-discover on next run
    client_.set_api_base(provider.api_base);
    client_.set_api_key(provider.api_key);
}

void ChatSession::set_lsp_client(LspClient* lsp) {
    lsp_client_ = lsp;
    if (lsp_client_) {
        // Wire the file-modified callback to sync modified files with the LSP server.
        // This ensures that after write_file or edit_file, clangd sees the new content.
        *file_modified_cb_ = [this](const std::string& path) {
            // Read file content from disk
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) {
                return; // silently skip — file may have been deleted
            }
            auto size = file.tellg();
            std::string content(static_cast<size_t>(size), '\0');
            file.seekg(0);
            file.read(content.data(), size);

            // Sync with LSP
            std::string uri = lsp::path_to_uri(path);
            auto lang = LspClient::language_id_from_extension(path);
            auto result = lsp_client_->ensure_file_synced(uri, lang, content);
            if (!result) {
                // Non-fatal: log and continue
                std::cerr << "notify_file_modified: failed to sync " << path
                          << " with LSP: " << result.error() << std::endl;
            }
        };

        tools_.add(make_get_lsp_diagnostics_tool(*lsp_client_));
        tools_.add(make_get_lsp_hover_tool(*lsp_client_));
        tools_.add(make_get_lsp_definition_tool(*lsp_client_));
        tools_.add(make_get_lsp_completion_tool(*lsp_client_));
        tools_.add(make_get_lsp_code_actions_tool(*lsp_client_));
        tools_.add(make_get_lsp_rename_tool(*lsp_client_));
        tools_.add(make_get_lsp_format_tool(*lsp_client_));
    }
}

void ChatSession::notify_file_modified(const std::string& path) {
    if (*file_modified_cb_) {
        (*file_modified_cb_)(path);
    }
}

void ChatSession::set_wiki(Wiki* wiki) {
    wiki_ = wiki;
    if (wiki_) {
        tools_.add(make_list_wiki_pages_tool(*wiki_));
        tools_.add(make_read_wiki_page_tool(*wiki_));
        tools_.add(make_write_wiki_page_tool(*wiki_));
        tools_.add(make_edit_wiki_page_tool(*wiki_));
        tools_.add(make_delete_wiki_page_tool(*wiki_));
    }
}

int ChatSession::context_usage_percent() const {
    if (context_limit_ <= 0) return 0;
    // Use the API-reported token count when available (more accurate),
    // fall back to the conversation estimate after restart.
    int tokens = last_usage_.total_tokens;
    if (tokens == 0) {
        tokens = static_cast<int>(conversation_.estimate_total_tokens());
    }
    return static_cast<int>(tokens * 100 / context_limit_);
}

Result<ChatResult> ChatSession::run_once(const std::string& user_input) {
    // ── Discover context limit (once per model/endpoint) ──
    if (!context_limit_discovered_) {
        static std::mutex cache_mutex;
        static std::unordered_map<std::string, int> cache;

        std::string cache_key = client_.url() + ":" + model_;
        {
            std::lock_guard<std::mutex> lock(cache_mutex);
            auto it = cache.find(cache_key);
            if (it != cache.end()) {
                context_limit_ = it->second;
                context_limit_discovered_ = true;
            }
        }
        if (!context_limit_discovered_) {
            int discovered = client_.fetch_model_context_limit(model_);
            std::lock_guard<std::mutex> lock(cache_mutex);
            if (discovered > 0) {
                cache[cache_key] = discovered;
                context_limit_ = discovered;
            }
            context_limit_discovered_ = true;
        }
    }

    // ── Single turn processing ──
    // Snapshot before adding this turn's prompt.  If anything goes wrong
    // we roll back to here.
    auto turn_snapshot = conversation_.message_count();

    // Add the user input as a message
    conversation_.add_user(user_input);

    std::string last_content;
    std::string last_reasoning;
    bool produced_content = false;

    try {
        for (int iter = 0; iter < max_iterations_; iter++) {
            // Build payload from the conversation.
            json payload = {{"model", model_},
                {"reasoning_effort", reasoning_effort_},
                {"messages", conversation_.build_openai_payload(system_prompt_)},
                {"tools", tools_.to_openai_tools()},
                {"stream", true}};
            payload["stream_options"] = {{"include_usage", true}};
            std::string content;
            std::string reasoning;
            ToolAccumulator tool_acc;
            bool stream_errored = false;
            std::string stream_error;

            auto on_data = [&](const json& data) {
                // Capture token usage if present (may appear in the final chunk).
                auto usage_it = data.find("usage");
                if (usage_it != data.end() && usage_it->is_object()) {
                    try {
                        last_usage_ = usage_it->get<Usage>();
                    } catch (...) {
                        // Ignore malformed usage data.
                    }
                }

                if (!data.contains("choices") || data["choices"].empty())
                    return;
                const auto& delta = data["choices"][0]["delta"];

                auto rc_it = delta.find("reasoning_content");
                if (rc_it != delta.end() && rc_it->is_string()) {
                    auto text = rc_it->get<std::string>();
                    reasoning += text;
                    if (output_cb_)
                        output_cb_(text, OutputType::Reasoning);
                }

                auto tc_it = delta.find("tool_calls");
                if (tc_it != delta.end() && tc_it->is_array()) {
                    tool_acc.apply(delta);
                }

                auto c_it = delta.find("content");
                if (c_it != delta.end() && c_it->is_string()) {
                    auto text = c_it->get<std::string>();
                    content += text;
                    if (output_cb_)
                        output_cb_(text, OutputType::Content);
                }
            };

            SSEParser::Callbacks callbacks({
                .on_data = on_data,
                .on_done = []() {},
                .on_error =
                    [&](const std::string& err) {
                        stream_errored = true;
                        stream_error = err;
                    },
            });

            auto stream_result = client_.stream_chat(payload, callbacks);

            if (!stream_result) {
                // Stream transport error — roll back only this turn
                conversation_.truncate_conversation(turn_snapshot);
                auto msg = stream_result.error();
                auto raw = client_.last_raw_response();
                if (!raw.empty()) {
                    msg += " | raw: " + raw.substr(0, 500);
                }
                return std::unexpected(std::move(msg));
            }

            if (stream_errored && content.empty()) {
                // Stream error with no content — roll back only this turn
                conversation_.truncate_conversation(turn_snapshot);
                return std::unexpected(stream_error);
            }

            auto calls = tool_acc.finalize();
            if (!calls.empty()) {
                auto msg_id = conversation_.add_assistant("", reasoning, calls);

                if (*cancelled_) {
                    // User cancelled — roll back only this turn
                    conversation_.truncate_conversation(turn_snapshot);
                    return std::unexpected("Interrupted during tool execution");
                }

                int remaining = max_iterations_ - iter - 1;

                if (calls.size() > 1) {
                    // If any tool in the batch has Write permission, serialize
                    // the batch to avoid race conditions on shared resources
                    // (e.g. .git/index.lock) that the per-tool mutex can't
                    // fully protect (e.g. git_add + git_commit with all:true).
                    auto write_tools = tools_.tool_names_by_permission(ToolPermission::Write);
                    bool has_write = false;
                    for (const auto& call : calls) {
                        if (write_tools.count(call.name)) {
                            has_write = true;
                            break;
                        }
                    }

                    if (has_write) {
                        // Serial execution for write tools
                        for (const auto& call : calls) {
                            if (*cancelled_) {
                                conversation_.truncate_conversation(turn_snapshot);
                                return std::unexpected("Interrupted during tool execution");
                            }
                            if (output_cb_) {
                                output_cb_(
                                    "\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                    OutputType::ToolInvocation);
                            }
                            Result<std::string> tr;
                            try {
                                tr = tools_.execute(call.name, call.arguments);
                            } catch (const std::exception& e) {
                                tr = std::unexpected(std::string(e.what()));
                            }
                            auto result = tr ? *tr : tr.error();
                            conversation_.add_tool(msg_id, call.id, result);
                        }
                    } else {
                        // Parallel execution for read-only tools
                        std::vector<std::future<Result<std::string>>> futures;
                        futures.reserve(calls.size());
                        for (size_t i = 0; i < calls.size(); i++) {
                            futures.push_back(std::async(std::launch::async,
                                [&, i] {
                                    return tools_.execute(calls[i].name, calls[i].arguments);
                                }));
                        }
                        for (size_t i = 0; i < calls.size(); i++) {
                            if (*cancelled_) {
                                conversation_.truncate_conversation(turn_snapshot);
                                return std::unexpected("Interrupted during tool execution");
                            }
                            if (output_cb_) {
                                output_cb_(
                                    "\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments +
                                        ")",
                                    OutputType::ToolInvocation);
                            }
                            Result<std::string> tr;
                            try {
                                tr = futures[i].get();
                            } catch (const std::exception& e) {
                                tr = std::unexpected(std::string(e.what()));
                            }
                            auto result = tr ? *tr : tr.error();
                            conversation_.add_tool(msg_id, calls[i].id, result);
                        }
                    }
                } else {
                    for (const auto& call : calls) {
                        if (*cancelled_) {
                            conversation_.truncate_conversation(turn_snapshot);
                            return std::unexpected("Interrupted during tool execution");
                        }
                        if (output_cb_) {
                            output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")",
                                OutputType::ToolInvocation);
                        }
                        Result<std::string> tr;
                        try {
                            tr = tools_.execute(call.name, call.arguments);
                        } catch (const std::exception& e) {
                            tr = std::unexpected(std::string(e.what()));
                        }
                        auto result = tr ? *tr : tr.error();
                        conversation_.add_tool(msg_id, call.id, result);
                    }
                }
                continue;
            }

            // No tool calls — content response.
            conversation_.add_assistant(content, reasoning);
            last_content = content;
            last_reasoning = reasoning;
            produced_content = true;
            break;
        }
    } catch (const std::exception& e) {
        conversation_.truncate_conversation(turn_snapshot);
        return std::unexpected(std::string(e.what()));
    }

    if (!produced_content) {
        std::string msg = "Tool call budget exhausted (" +
            std::to_string(max_iterations_) + " iterations).";
        conversation_.add_assistant(msg, "");
        last_content = msg;
        last_reasoning = "";
        produced_content = true;
    }

    // ── Auto-compaction at 90% context usage ──
    if (context_usage_percent() >= 90) {
        auto compact_result = compact();
        if (!compact_result) {
            // Compaction failed — log but don't fail the overall turn
            if (output_cb_)
                output_cb_("compact() failed: " + compact_result.error(), OutputType::ToolInvocation);
        }
    }

    return ChatResult{std::move(last_content), std::move(last_reasoning)};
}

Result<void> ChatSession::compact() {
    // Snapshot the current messages (skip system prompt — that's added at build time)
    auto& msgs = conversation_.messages();

    // Don't compact if there's nothing meaningful
    if (msgs.size() < 2) {
        return {}; // Nothing to compact
    }

    // Build a summarization prompt from the conversation
    std::string summary_prompt = "Please provide a comprehensive summary of the following conversation. "
        "Preserve all important context, decisions, code changes, file paths, "
        "and outstanding tasks. Be detailed enough that the conversation can "
        "continue seamlessly from this summary.\n\n---\n";

    for (const auto& msg : msgs) {
        if (msg.role == "system") continue; // skip system messages

        summary_prompt += "[" + msg.role + "]: ";
        if (msg.content.has_value()) {
            summary_prompt += *msg.content;
        }
        if (!msg.reasoning_content.empty()) {
            summary_prompt += "\n[reasoning]: " + msg.reasoning_content;
        }
        for (const auto& tc : msg.tool_calls) {
            summary_prompt += "\n[tool_call: " + tc.name + "(" + tc.arguments + ")]";
            if (!tc.result.empty()) {
                summary_prompt += "\n[tool_result: " + tc.result + "]";
            }
        }
        summary_prompt += "\n";
    }

    // Send to LLM via client_
    json payload = {
        {"model", model_},
        {"reasoning_effort", reasoning_effort_},
        {"messages", json::array({
            {{"role", "system"}, {"content", sanitize_utf8(system_prompt_)}},
            {{"role", "user"}, {"content", sanitize_utf8(summary_prompt)}}
        })},
        {"stream", true}
    };
    payload["stream_options"] = {{"include_usage", true}};

    std::string summary;
    bool stream_errored = false;
    std::string stream_error;

    auto on_data = [&](const json& data) {
        if (!data.contains("choices") || data["choices"].empty())
            return;
        const auto& delta = data["choices"][0]["delta"];

        auto c_it = delta.find("content");
        if (c_it != delta.end() && c_it->is_string()) {
            summary += c_it->get<std::string>();
        }
    };

    SSEParser::Callbacks callbacks({
        .on_data = on_data,
        .on_done = []() {},
        .on_error = [&](const std::string& err) {
            stream_errored = true;
            stream_error = err;
        },
    });

    auto stream_result = client_.stream_chat(payload, callbacks);
    if (!stream_result) {
        return std::unexpected(stream_result.error());
    }
    if (stream_errored && summary.empty()) {
        return std::unexpected(stream_error);
    }

    // Replace the conversation with just the summary
    conversation_.replace_with_summary(summary);

    if (output_cb_)
        output_cb_("Conversation compacted.", OutputType::ToolInvocation);

    return {};
}
