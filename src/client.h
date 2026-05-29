#pragma once

#include "config.h"
#include "types.h"

#include <curl/curl.h>

#include <string>
#include <vector>

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

    void set_cancelled(CancellationToken t) { cancelled_ = std::move(t); }

    void set_api_base(const std::string& base) { api_base_ = base; }
    void set_api_key(const std::string& key) { api_key_ = key; }
    void set_api_type(const std::string& t) { api_type_ = t; }
    const std::string& api_type() const { return api_type_; }

    Result<json> chat(const json& payload);
    Result<void> stream_chat(const json& payload, SSEParser::Callbacks callbacks);

    // Query the API for model metadata, returning the context window size if found.
    // Returns 0 if the endpoint doesn't expose it (caller should use a default).
    int fetch_model_context_limit(const std::string& model);

    // Query /v1/models and return the list of model IDs (empty on error).
    Result<std::vector<std::string>> fetch_models();

    const std::string& last_raw_response() const { return raw_response_; }
    std::string url() const {
        // Anthropic endpoints use just /messages under the base path.
        // The /v1 prefix is part of api.anthropic.com's URL structure,
        // not a universal Anthropic convention — other providers
        // (opencode, ofox.ai) use different path prefixes. The user
        // sets api_base to include any path prefix they need.
        if (api_type_ == "anthropic")
            return api_base_ + "/messages";
        return api_base_ + "/chat/completions";
    }
    std::string models_url() const { return api_base_ + "/models"; }

    /// Stream an Anthropic-format response (named SSE events).
    Result<void> stream_chat_anthropic(const json& payload, SSEParser::Callbacks callbacks);

  private:
    static constexpr int kMaxRetries = 3;
    static constexpr double kBaseDelaySec = 1.0;

    struct curl_slist* make_headers() const;
    bool should_retry(long http_code) const;
    CURLcode perform_with_retry(CURL* curl, long& http_code, std::string& body);

    // Low-level HTTP GET helper
    Result<std::string> http_get(const std::string& url);

    CancellationToken cancelled_;
    std::string api_base_;
    std::string api_key_;
    std::string api_type_ = "openai";
    std::string raw_response_;

    static size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata);
    static size_t write_stream(char* ptr, size_t size, size_t nmemb, void* userdata);
};
