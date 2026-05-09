#include "client.h"

#include <curl/curl.h>
#include <cstring>
#include <iostream>
#include <random>
#include <thread>
#include <chrono>

ChatClient::ChatClient(std::string api_base, std::string api_key)
    : api_base_(std::move(api_base)), api_key_(std::move(api_key)) {
    while (!api_base_.empty() && api_base_.back() == '/') {
        api_base_.pop_back();
    }
}

ChatClient::~ChatClient() = default;

struct curl_slist* ChatClient::make_headers() const {
    struct curl_slist* list = nullptr;
    list = curl_slist_append(list, "Content-Type: application/json");
    if (!api_key_.empty()) {
        std::string auth_header = "Authorization: Bearer " + api_key_;
        list = curl_slist_append(list, auth_header.c_str());
    }
    return list;
}

size_t ChatClient::write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t ChatClient::write_stream(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* parser = static_cast<SSEParser*>(userdata);
    parser->feed(ptr, size * nmemb);
    return size * nmemb;
}

static int progress_cb(void* /*clientp*/,
    curl_off_t /*dltotal*/,
    curl_off_t /*dlnow*/,
    curl_off_t /*ultotal*/,
    curl_off_t /*ulnow*/) {
    return g_interrupted ? 1 : 0;
}

static CURL* setup_curl(
    const std::string& url, struct curl_slist* headers, const std::string& payload_str) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload_str.size()));

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L);

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "llm-chat/0.1");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);

    // Enable SSL/TLS verification when using HTTPS.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Use system default CA bundle — do NOT set a custom CA path so that
    // curl finds the system trust store automatically.
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);

    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    return curl;
}

bool ChatClient::should_retry(long http_code) const {
    return http_code == 429 || (http_code >= 500 && http_code < 600);
}

/// Returns a random delay in [0.5*base, 1.5*base] to add jitter to retries.
static double jittered_delay(double base_sec) {
    // thread_local so we seed once per thread (fine for single-threaded UI)
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(0.5, 1.5);
    return base_sec * dist(rng);
}

CURLcode ChatClient::perform_with_retry(CURL* curl, long& http_code, std::string& body) {
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        body.clear();
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

        CURLcode res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && !should_retry(http_code))
            return CURLE_OK;

        if (attempt == kMaxRetries - 1)
            return res;

        bool recoverable = (res == CURLE_OK && should_retry(http_code)) ||
            res == CURLE_SEND_ERROR || res == CURLE_RECV_ERROR || res == CURLE_OPERATION_TIMEDOUT ||
            res == CURLE_COULDNT_CONNECT;
        if (!recoverable)
            return res;

        std::this_thread::sleep_for(
            std::chrono::duration<double>(jittered_delay(kBaseDelaySec * (1 << attempt))));
    }
    return CURLE_OK;
}

Result<json> ChatClient::chat(const json& payload) {
    std::string payload_str = payload.dump();
    std::string body;
    long http_code = 0;

    auto* headers = make_headers();
    CURL* curl = setup_curl(url(), headers, payload_str);
    if (!curl) {
        curl_slist_free_all(headers);
        return std::unexpected(std::string("curl_easy_init failed"));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    CURLcode res = perform_with_retry(curl, http_code, body);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    raw_response_ = body;

    if (res != CURLE_OK) {
        return std::unexpected(std::string("curl error: ") + curl_easy_strerror(res));
    }

    if (http_code != 200) {
        // Log the full response body to stderr for debugging.
        if (!body.empty()) {
            std::cerr << "HTTP " << http_code << " response body:\n" << body << std::endl;
        }
        std::string msg = "HTTP " + std::to_string(http_code);
        if (!body.empty()) {
            msg += ": " + body.substr(0, 500);
        }
        return std::unexpected(msg);
    }

    try {
        return json::parse(body);
    } catch (const json::parse_error& e) {
        return std::unexpected("JSON parse error: " + std::string(e.what()));
    }
}

Result<void> ChatClient::stream_chat(const json& payload, SSEParser::Callbacks callbacks) {
    std::string payload_str = payload.dump();
    long http_code = 0;

    // Wrap callbacks to detect if any data was already delivered to the user.
    // We must not retry if callbacks were invoked — retrying would duplicate data.
    bool data_delivered = false;
    SSEParser::Callbacks guarded;
    if (callbacks.on_data) {
        guarded.on_data = [&data_delivered, cb = std::move(callbacks.on_data)](const json& j) {
            data_delivered = true;
            cb(j);
        };
    }
    if (callbacks.on_done) {
        guarded.on_done = [&data_delivered, cb = std::move(callbacks.on_done)]() {
            data_delivered = true;
            cb();
        };
    }
    if (callbacks.on_error) {
        guarded.on_error = [&data_delivered, cb = std::move(callbacks.on_error)](
                               const std::string& s) {
            data_delivered = true;
            cb(s);
        };
    }

    SSEParser parser(std::move(guarded));

    auto* headers = make_headers();
    CURL* curl = setup_curl(url(), headers, payload_str);
    if (!curl) {
        curl_slist_free_all(headers);
        return std::unexpected(std::string("curl_easy_init failed"));
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);

    CURLcode res = CURLE_OK;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        data_delivered = false;
        parser.reset();
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);

        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && !should_retry(http_code))
            break;

        if (attempt == kMaxRetries - 1)
            break;

        // If any callback was already invoked, data was consumed by the user.
        // Retrying would produce duplicate content — bail out instead.
        if (data_delivered)
            break;

        bool recoverable = (res == CURLE_OK && should_retry(http_code)) ||
            res == CURLE_SEND_ERROR || res == CURLE_RECV_ERROR || res == CURLE_OPERATION_TIMEDOUT ||
            res == CURLE_COULDNT_CONNECT;
        if (!recoverable)
            break;

        std::this_thread::sleep_for(
            std::chrono::duration<double>(jittered_delay(kBaseDelaySec * (1 << attempt))));
    }

    parser.flush();
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    raw_response_ = parser.raw();

    if (res != CURLE_OK) {
        // Log the full raw response to stderr for debugging.
        if (!raw_response_.empty()) {
            std::cerr << "curl error raw response (" << curl_easy_strerror(res) << "):\n"
                      << raw_response_ << std::endl;
        }
        auto msg = std::string("curl error: ") + curl_easy_strerror(res);
        if (!raw_response_.empty()) {
            msg += " | raw: " + raw_response_.substr(0, 500);
        }
        return std::unexpected(std::move(msg));
    }

    if (http_code != 200) {
        // Log the full raw response to stderr for debugging.
        if (!raw_response_.empty()) {
            std::cerr << "HTTP " << http_code << " response body:\n" << raw_response_ << std::endl;
        }
        auto msg = "HTTP " + std::to_string(http_code);
        if (!raw_response_.empty()) {
            msg += ": " + raw_response_.substr(0, 500);
        }
        return std::unexpected(msg);
    }

    return {};
}
