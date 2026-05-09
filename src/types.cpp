#include "types.h"

#include <unordered_set>

// ---------------------------------------------------------------------------
// UTF-8 sanitization — replace invalid byte sequences with U+FFFD
// ---------------------------------------------------------------------------

std::string sanitize_utf8(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        auto b = static_cast<unsigned char>(s[i]);

        if (b <= 0x7F) {
            result += s[i];
            i += 1;
        } else if (b >= 0xC2 && b <= 0xDF && i + 1 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(s[i + 1]) <= 0xBF) {
            result.append(s, i, 2); // Optimized
            i += 2;
        } else if (b >= 0xE0 && b <= 0xEF && i + 2 < s.size()) {
            auto c1 = static_cast<unsigned char>(s[i + 1]);
            auto c2 = static_cast<unsigned char>(s[i + 2]);
            bool ok = (c1 >= 0x80 && c1 <= 0xBF && c2 >= 0x80 && c2 <= 0xBF);
            if (b == 0xE0)
                ok = ok && (c1 >= 0xA0);
            if (b == 0xED)
                ok = ok && (c1 <= 0x9F);

            if (ok) {
                result.append(s, i, 3); // Optimized
                i += 3;
            } else {
                result += "\xEF\xBF\xBD";
                i += 1;
            }
        } else if (b >= 0xF0 && b <= 0xF4 && i + 3 < s.size()) {
            auto c1 = static_cast<unsigned char>(s[i + 1]);
            auto c2 = static_cast<unsigned char>(s[i + 2]);
            auto c3 = static_cast<unsigned char>(s[i + 3]);
            bool ok =
                (c1 >= 0x80 && c1 <= 0xBF && c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF);
            if (b == 0xF0)
                ok = ok && (c1 >= 0x90);
            if (b == 0xF4)
                ok = ok && (c1 <= 0x8F);

            if (ok) {
                result.append(s, i, 4); // Optimized
                i += 4;
            } else {
                result += "\xEF\xBF\xBD";
                i += 1;
            }
        } else {
            result += "\xEF\xBF\xBD";
            i += 1;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Usage JSON deserialization
// ---------------------------------------------------------------------------

void from_json(const json& j, Usage& u) {
    u.prompt_tokens = j.value("prompt_tokens", 0);
    u.completion_tokens = j.value("completion_tokens", 0);
    u.total_tokens = j.value("total_tokens", 0);
}

// ---------------------------------------------------------------------------
// ToolCall JSON serialization
// ---------------------------------------------------------------------------

void to_json(json& j, const ToolCall& tc) {
    j = json{
        {"index", tc.index},
        {"id", tc.id},
        {"name", tc.name},
        {"arguments", tc.arguments},
    };
}

// ---------------------------------------------------------------------------
// Token estimation (fast approximate)
// ---------------------------------------------------------------------------

size_t estimate_tokens(const std::string& text) {
    // GPT-style BPE approximation: ~0.25 tokens/char for English,
    // ~0.15 for code. Use 0.25 as a safe overestimate.
    // Add 1 for message framing overhead (role, newlines).
    return (text.size() + 3) / 4 + 1;
}

size_t estimate_tokens(const Message& msg) {
    size_t t = estimate_tokens(msg.content.value_or(""));
    t += estimate_tokens(msg.reasoning_content);
    for (const auto& tc : msg.tool_calls) {
        t += estimate_tokens(tc.id);
        t += estimate_tokens(tc.name);
        t += estimate_tokens(tc.arguments);
    }
    t += estimate_tokens(msg.tool_call_id);
    // JSON framing overhead per message (~20 tokens for role + surrounding keys)
    t += 20;
    return t;
}

// ---------------------------------------------------------------------------
// ToolAccumulator
// ---------------------------------------------------------------------------

void ToolAccumulator::apply(const json& delta) {
    auto it = delta.find("tool_calls");
    if (it == delta.end() || !it->is_array()) {
        return;
    }

    for (const auto& tc : *it) {
        int idx = tc.value("index", 0);
        auto& call = calls_[idx];
        call.index = idx;

        auto id_it = tc.find("id");
        if (id_it != tc.end() && id_it->is_string()) {
            call.id = id_it->get<std::string>();
        }

        auto func_it = tc.find("function");
        if (func_it != tc.end() && func_it->is_object()) {
            auto name_it = func_it->find("name");
            if (name_it != func_it->end() && name_it->is_string()) {
                call.name = name_it->get<std::string>();
            }
            auto args_it = func_it->find("arguments");
            if (args_it != func_it->end() && args_it->is_string()) {
                call.arguments += args_it->get<std::string>();
            }
        }
    }
}

std::vector<ToolCall> ToolAccumulator::finalize() const {
    std::vector<ToolCall> result;
    result.reserve(calls_.size());
    for (const auto& [idx, tc] : calls_) {
        result.push_back(tc);
    }
    return result;
}

// ---------------------------------------------------------------------------
// SSEParser
// ---------------------------------------------------------------------------

SSEParser::SSEParser(Callbacks cb) : cb_(std::move(cb)) {}

void SSEParser::feed(const char* data, size_t len) {
    raw_.append(data, len);
    buf_.append(data, len);

    while (true) {
        auto nl = buf_.find('\n');
        if (nl == std::string::npos) {
            break; // incomplete line, wait for more data
        }

        std::string line = buf_.substr(0, nl);
        buf_.erase(0, nl + 1); // remove line + \n
        process_line(std::move(line));
    }
}

void SSEParser::process_line(std::string line) {
    // Strip trailing \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    // Skip empty lines (event separator)
    if (line.empty()) {
        return;
    }

    constexpr std::string_view prefix = "data: ";
    if (line.size() > prefix.size() && std::string_view(line).substr(0, prefix.size()) == prefix) {
        std::string payload = line.substr(prefix.size());

        if (payload == "[DONE]") {
            try {
                if (cb_.on_done) {
                    cb_.on_done();
                }
            } catch (...) {
                // swallow all exceptions — must not throw through C frames (libcurl)
            }
            return;
        }

        try {
            json j = json::parse(payload);
            if (cb_.on_data) {
                cb_.on_data(j);
            }
        } catch (const std::exception& e) {
            if (cb_.on_error) {
                cb_.on_error(std::string("SSE error: ") + e.what() + " | payload: " + payload);
            }
        } catch (...) {
            if (cb_.on_error) {
                cb_.on_error("SSE error: unknown exception | payload: " + payload);
            }
        }
        return;
    }

    // Ignore other fields (event:, :keepalive, etc.)
}

void SSEParser::flush() {
    if (!buf_.empty()) {
        process_line(std::move(buf_));
        buf_.clear();
    }
}

void SSEParser::reset() {
    buf_.clear();
    raw_.clear();
}

// ---------------------------------------------------------------------------
// Conversation
// ---------------------------------------------------------------------------

Conversation::Conversation(std::string system_prompt)
    : system_prompt_(std::move(system_prompt)) {}

void Conversation::add_user(std::string content) {
    messages_.push_back(Message{
        .role = "user",
        .content = std::move(content),
        .retain = RetentionClass::Preserve,
    });
}

void Conversation::add_assistant(
    std::string content, std::string reasoning, std::vector<ToolCall> tool_calls) {
    // Before adding the new assistant message, mark any superseded tool-call
    // rounds as Summarizable. If this is a content-bearing response (final
    // answer), the previous tool-call-only assistant + its tool results are
    // no longer needed for future context — they can be dropped or summarized.
    if (!content.empty()) {
        mark_superseded_tool_calls();
    }

    Message msg;
    msg.role = "assistant";
    msg.reasoning_content = std::move(reasoning);

    if (!tool_calls.empty()) {
        msg.content = std::nullopt;
        msg.tool_calls = std::move(tool_calls);
        // If the assistant message only contains tool calls (no content yet),
        // it's a mid-loop thinking step — summarizable.
        msg.retain = RetentionClass::Summarizable;
    } else {
        msg.content = std::move(content);
        msg.retain = RetentionClass::Preserve;
    }

    messages_.push_back(std::move(msg));
}

void Conversation::add_tool(const std::string& tool_call_id, const std::string& content) {
    messages_.push_back(Message{
        .role = "tool",
        .content = content,
        .tool_call_id = tool_call_id,
        .retain = RetentionClass::Droppable,
    });
}

void Conversation::mark_superseded_tool_calls() {
    // Walk backwards from the end. When we find a content-bearing assistant
    // message, we stop — everything before it that's a tool-call round can
    // be demoted.
    bool found_content = false;
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->role == "assistant" && it->content.has_value() && !it->content->empty()) {
            // This is a final answer. Everything before it is superseded.
            found_content = true;
            continue;
        }
        if (!found_content) continue; // we haven't hit the final answer yet

        if (it->role == "assistant" && !it->tool_calls.empty()) {
            // This assistant triggered tool calls that have now been answered.
            // Downgrade from Preserve to Summarizable.
            if (it->retain == RetentionClass::Preserve) {
                it->retain = RetentionClass::Summarizable;
            }
            // Mark the corresponding tool results as Droppable.
            for (const auto& tc : it->tool_calls) {
                for (auto& msg : messages_) {
                    if (msg.role == "tool" && msg.tool_call_id == tc.id &&
                        msg.retain == RetentionClass::Preserve) {
                        msg.retain = RetentionClass::Droppable;
                    }
                }
            }
        }
    }
}

void Conversation::truncate(size_t n) {
    if (n < messages_.size()) {
        messages_.resize(n);
    }
}

void Conversation::clear() { messages_.clear(); }

size_t Conversation::estimate_total_tokens() const {
    size_t t = 0;
    for (const auto& msg : messages_) {
        t += estimate_tokens(msg);
    }
    return t;
}

bool Conversation::needs_compaction(size_t context_limit, size_t compact_threshold_pct) const {
    if (context_limit == 0) return false;
    size_t threshold = context_limit * compact_threshold_pct / 100;
    return estimate_total_tokens() > threshold;
}

size_t Conversation::compact(size_t context_limit, size_t compact_threshold_pct) {
    if (context_limit == 0) return 0;

    size_t before = estimate_total_tokens();
    size_t budget = context_limit * compact_threshold_pct / 100;

    if (before <= budget) return 0;

    // ── Phase 1: Drop Droppable messages ──
    // Remove all messages tagged Droppable — these are completed tool results
    // and old reasoning content that the model no longer needs.
    {
        for (auto it = messages_.begin(); it != messages_.end();) {
            if (it->retain == RetentionClass::Droppable) {
                it = messages_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (estimate_total_tokens() <= budget) {
        return before - estimate_total_tokens();
    }

    // ── Phase 2: Summarize old exchanges ──
    // If we have a summary callback, find the oldest contiguous run of
    // Summarizable messages and condense them into a single summary message.
    if (summary_cb_) {
        // Find the oldest contiguous Summarizable run (skip index 0 = system)
        int64_t run_start = -1;
        int64_t run_end = -1;
        for (size_t i = 1; i < messages_.size(); i++) {
            if (messages_[i].retain == RetentionClass::Summarizable) {
                if (run_start < 0) run_start = static_cast<int64_t>(i);
                run_end = static_cast<int64_t>(i) + 1;
            } else if (run_start >= 0) {
                break; // only compact the oldest run
            }
        }

        if (run_start >= 0 && run_end > run_start) {
            std::vector<Message> to_summarize(
                messages_.begin() + run_start, messages_.begin() + run_end);

            // Compute a token budget for the summary (10% of total budget)
            size_t summary_budget = std::max(budget / 10, size_t(256));
            auto summary = summary_cb_(to_summarize, summary_budget);
            if (summary.has_value()) {
                Message summary_msg;
                summary_msg.role = "user";
                summary_msg.content =
                    "[Previous exchanges summarized: " + *summary + "]";
                summary_msg.retain = RetentionClass::Preserve;
                summary_msg.is_summary = true;

                messages_.erase(
                    messages_.begin() + run_start, messages_.begin() + run_end);
                messages_.insert(
                    messages_.begin() + run_start, std::move(summary_msg));
            }
        }
    }

    if (estimate_total_tokens() <= budget) {
        return before - estimate_total_tokens();
    }

    // ── Phase 3: Sliding window (last resort) ──
    // If still over budget, drop the oldest non-system, non-Preserve messages.
    {
        while (estimate_total_tokens() > budget && messages_.size() > 2) {
            bool dropped = false;
            for (size_t i = 1; i < messages_.size(); i++) {
                if (messages_[i].retain != RetentionClass::Preserve) {
                    messages_.erase(messages_.begin() + static_cast<int64_t>(i));
                    dropped = true;
                    break;
                }
            }
            if (!dropped) break;
        }
    }

    // ── Phase 4: Remove orphaned tool_calls messages ──
    // If all tool results for a given assistant tool_calls message were dropped
    // in previous phases, remove the orphaned assistant message too. The API
    // requires every assistant tool_calls message to have corresponding tool
    // responses.
    if (!messages_.empty()) {
        std::unordered_set<std::string> active_tool_ids;
        for (const auto& msg : messages_) {
            if (msg.role == "tool" && !msg.tool_call_id.empty()) {
                active_tool_ids.insert(msg.tool_call_id);
            }
        }
        for (auto it = messages_.begin(); it != messages_.end();) {
            if (it->role == "assistant" && !it->tool_calls.empty()) {
                bool any_alive = false;
                for (const auto& tc : it->tool_calls) {
                    if (active_tool_ids.count(tc.id)) {
                        any_alive = true;
                        break;
                    }
                }
                if (!any_alive) {
                    it = messages_.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }

    return before - estimate_total_tokens();
}

json Conversation::to_openai_messages() const {
    json arr = json::array();

    arr.push_back({{"role", "system"}, {"content", system_prompt_}});

    for (const auto& msg : messages_) {
        json j;
        j["role"] = msg.role;

        if (msg.role == "assistant" && !msg.tool_calls.empty()) {
            j["content"] = nullptr;
            json arr = json::array();
            for (const auto& tc : msg.tool_calls) {
                arr.push_back({{"id", sanitize_utf8(tc.id)},
                    {"type", "function"},
                    {"function",
                        {{"name", sanitize_utf8(tc.name)},
                            {"arguments", sanitize_utf8(tc.arguments)}}}});
            }
            j["tool_calls"] = std::move(arr);
            j["reasoning_content"] = sanitize_utf8(msg.reasoning_content);
        } else {
            j["content"] = sanitize_utf8(msg.content.value_or(""));
            if (!msg.reasoning_content.empty()) {
                j["reasoning_content"] = sanitize_utf8(msg.reasoning_content);
            }
        }

        if (!msg.tool_call_id.empty()) {
            j["tool_call_id"] = sanitize_utf8(msg.tool_call_id);
        }

        arr.push_back(std::move(j));
    }

    return arr;
}
