#include "types.h"

#include <algorithm>
#include <unordered_set>

// ---------------------------------------------------------------------------
// UTF-8 sanitization — replace invalid byte sequences with U+FFFD.
// Validates structural UTF-8 AND semantic constraints:
//   - Rejects overlong encodings (e.g. C0 BF for U+007F)
//   - Rejects encoded UTF-16 surrogates (ED A0 80 .. ED BF BF)
//   - Rejects codepoints above U+10FFFF (F4 90 ..)
// Each invalid byte (lead or continuation) is replaced by one U+FFFD.
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
    std::sort(result.begin(), result.end(), [](const ToolCall& a, const ToolCall& b) {
        return a.index < b.index;
    });
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
