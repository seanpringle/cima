#pragma once

#include "config.h"
#include "types.h"

#include <atomic>
#include <string>

extern std::atomic<bool> g_interrupted;

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class ChatClient {
public:
    ChatClient(std::string api_base, std::string api_key = {});
    ~ChatClient();

    ChatClient(const ChatClient&) = delete;
    ChatClient& operator=(const ChatClient&) = delete;
    ChatClient(ChatClient&&) = delete;
    ChatClient& operator=(ChatClient&&) = delete;

    Result<json> chat(const json& payload);
    Result<void> stream_chat(const json& payload, SSEParser::Callbacks callbacks);

    const std::string& last_raw_response() const { return raw_response_; }
    std::string url() const {
        return api_base_ + "/chat/completions";
    }

private:
    std::string api_base_;
    std::string api_key_;
    std::string raw_response_;

    struct curl_slist* make_headers() const;

    static size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t write_stream(char* ptr, size_t size, size_t nmemb, void* userdata);
};
