#include "types.h"

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
               static_cast<unsigned char>(s[i + 1]) >= 0x80 && static_cast<unsigned char>(s[i + 1]) <= 0xBF) {
      result += s.substr(i, 2);
      i += 2;
    } else if (b >= 0xE0 && b <= 0xEF && i + 2 < s.size()) {
      auto c1 = static_cast<unsigned char>(s[i + 1]);
      auto c2 = static_cast<unsigned char>(s[i + 2]);
      bool ok = (c1 >= 0x80 && c1 <= 0xBF && c2 >= 0x80 && c2 <= 0xBF);
      if (b == 0xE0) ok = ok && (c1 >= 0xA0);
      if (b == 0xED) ok = ok && (c1 <= 0x9F);
      if (ok) {
        result += s.substr(i, 3);
        i += 3;
      } else {
        result += "\xEF\xBF\xBD";
        i += 1;
      }
    } else if (b >= 0xF0 && b <= 0xF4 && i + 3 < s.size()) {
      auto c1 = static_cast<unsigned char>(s[i + 1]);
      auto c2 = static_cast<unsigned char>(s[i + 2]);
      auto c3 = static_cast<unsigned char>(s[i + 3]);
      bool ok = (c1 >= 0x80 && c1 <= 0xBF && c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF);
      if (b == 0xF0) ok = ok && (c1 >= 0x90);
      if (b == 0xF4) ok = ok && (c1 <= 0x8F);
      if (ok) {
        result += s.substr(i, 4);
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
      if (cb_.on_done) {
        cb_.on_done();
      }
      return;
    }

    try {
      json j = json::parse(payload);
      if (cb_.on_data) {
        cb_.on_data(j);
      }
    } catch (const json::exception& e) {
      if (cb_.on_error) {
        cb_.on_error(std::string("JSON error: ") + e.what() + " | payload: " + payload);
      }
    }
    return;
  }

  // Ignore other fields (event:, :keepalive, etc.)
}

void SSEParser::reset() { buf_.clear(); }

// ---------------------------------------------------------------------------
// Conversation
// ---------------------------------------------------------------------------

Conversation::Conversation(std::string system_prompt) : system_prompt_(std::move(system_prompt)) {}

void Conversation::add_user(std::string content) { messages_.push_back(Message{.role = "user", .content = std::move(content)}); }

void Conversation::add_assistant(std::string content, std::string reasoning, std::vector<ToolCall> tool_calls) {
  Message msg;
  msg.role = "assistant";
  msg.reasoning_content = std::move(reasoning);

  if (!tool_calls.empty()) {
    msg.content = std::nullopt;
    msg.tool_calls = std::move(tool_calls);
  } else {
    msg.content = std::move(content);
  }

  messages_.push_back(std::move(msg));
}

void Conversation::add_tool(const std::string& tool_call_id, const std::string& content) {
  messages_.push_back(Message{.role = "tool", .content = content, .tool_call_id = tool_call_id});
}

void Conversation::truncate(size_t n) {
  if (n < messages_.size()) {
    messages_.resize(n);
  }
}

void Conversation::clear() { messages_.clear(); }

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
        arr.push_back({{"id", sanitize_utf8(tc.id)}, {"type", "function"}, {"function", {{"name", sanitize_utf8(tc.name)}, {"arguments", sanitize_utf8(tc.arguments)}}}});
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
