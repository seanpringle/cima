#include "chat.h"

#include <future>

ChatSession::ChatSession(Config config)
    : model_(std::move(config.model)), safe_dir_(std::move(config.safe_dir)), max_iterations_(config.max_tool_iterations), conversation_(std::move(config.system_prompt)),
      client_(std::move(config.api_base), std::move(config.api_key)) {
  tools_.add_defaults(safe_dir_);
}

void ChatSession::clear() { conversation_.clear(); }

Result<ChatResult> ChatSession::run_once(const std::string& user_input) {
  auto snapshot = conversation_.size();
  conversation_.add_user(user_input);

  for (int iter = 0; iter < max_iterations_; iter++) {
    json payload = {{"model", model_}, {"messages", conversation_.to_openai_messages()}, {"tools", tools_.to_openai_tools()}, {"stream", true}};

    std::string content;
    std::string reasoning;
    ToolAccumulator tool_acc;
    bool stream_errored = false;
    std::string stream_error;

    SSEParser::Callbacks callbacks({
        .on_data =
            [&](const json& data) {
              if (!data.contains("choices") || data["choices"].empty())
                return;
              const auto& delta = data["choices"][0]["delta"];

              auto rc_it = delta.find("reasoning_content");
              if (rc_it != delta.end() && rc_it->is_string()) {
                auto text = sanitize_utf8(rc_it->get<std::string>());
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
                auto text = sanitize_utf8(c_it->get<std::string>());
                content += text;
                if (output_cb_)
                  output_cb_(text, OutputType::Content);
              }
            },
        .on_done = []() {},
        .on_error =
            [&](const std::string& err) {
              stream_errored = true;
              stream_error = err;
            },
    });

    auto stream_result = client_.stream_chat(payload, callbacks);

    if (!stream_result) {
      conversation_.truncate(snapshot);
      auto msg = stream_result.error();
      auto raw = client_.last_raw_response();
      if (!raw.empty()) {
        msg += " | raw: " + raw.substr(0, 500);
      }
      return std::unexpected(std::move(msg));
    }

    if (stream_errored && content.empty()) {
      conversation_.truncate(snapshot);
      return std::unexpected(stream_error);
    }

    auto calls = tool_acc.finalize();
    if (!calls.empty()) {
      conversation_.add_assistant("", reasoning, calls);

      if (g_interrupted) {
        conversation_.truncate(snapshot);
        return std::unexpected("Interrupted during tool execution");
      }

      if (calls.size() > 1) {
        std::vector<std::future<Result<std::string>>> futures;
        futures.reserve(calls.size());
        for (size_t i = 0; i < calls.size(); i++) {
          futures.push_back(std::async(std::launch::async, [&, i] {
            return tools_.execute(calls[i].name, calls[i].arguments);
          }));
        }
        for (size_t i = 0; i < calls.size(); i++) {
          if (g_interrupted) {
            conversation_.truncate(snapshot);
            return std::unexpected("Interrupted during tool execution");
          }
          if (output_cb_) {
            output_cb_("\xE2\x86\x92 " + calls[i].name + "(" + calls[i].arguments + ")", OutputType::ToolInvocation);
          }
          auto tr = futures[i].get();
          conversation_.add_tool(calls[i].id, tr ? *tr : tr.error());
        }
      } else {
        for (const auto& call : calls) {
          if (g_interrupted) {
            conversation_.truncate(snapshot);
            return std::unexpected("Interrupted during tool execution");
          }
          if (output_cb_) {
            output_cb_("\xE2\x86\x92 " + call.name + "(" + call.arguments + ")", OutputType::ToolInvocation);
          }
          auto tr = tools_.execute(call.name, call.arguments);
          conversation_.add_tool(call.id, tr ? *tr : tr.error());
        }
      }
      continue;
    }

    conversation_.add_assistant(content, reasoning);
    return ChatResult{std::move(content), std::move(reasoning)};
  }

  conversation_.truncate(snapshot);
  return std::unexpected("Maximum tool call iterations (" + std::to_string(max_iterations_) + ") reached");
}
