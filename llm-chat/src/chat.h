#pragma once

#include "client.h"
#include "config.h"
#include "tools.h"
#include "types.h"

struct ChatResult {
    std::string content;
    std::string reasoning;
};

class ChatSession {
public:
    explicit ChatSession(Config config);

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;
    ChatSession(ChatSession&&) = delete;
    ChatSession& operator=(ChatSession&&) = delete;

    Result<ChatResult> run_once(const std::string& user_input);
    void set_model(const std::string& m) { model_ = m; }
    void clear();
    const std::string& model() const { return model_; }

private:
    std::string model_;
    std::string safe_dir_;
    Conversation conversation_;
    ChatClient client_;
    ToolRegistry tools_;
};
