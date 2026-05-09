#pragma once

#include "config.h"
#include "types.h"

#include <curl/curl.h>

#include <string>

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
    std::string url() const { return api_base_ + "/chat/completions"; }

  private:
    static constexpr int kMaxRetries = 3;
    static constexpr double kBaseDelaySec = 1.0;

    struct curl_slist* make_headers() const;
    bool should_retry(long http_code) const;
    CURLcode perform_with_retry(CURL* curl, long& http_code, std::string& body);

    std::string api_base_;
    std::string api_key_;
    std::string raw_response_;

    static size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t write_stream(char* ptr, size_t size, size_t nmemb, void* userdata);
};
