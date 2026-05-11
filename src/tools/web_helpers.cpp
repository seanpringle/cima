#include "tools.h"

#include <curl/curl.h>
#include <mutex>
#include <string>
#include <unordered_map>

// ===================================================================
// Web search tool (libcurl)
// ===================================================================

size_t web_search_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

int web_search_progress_cb(void* clientp,
    curl_off_t /*dltotal*/,
    curl_off_t /*dlnow*/,
    curl_off_t /*ultotal*/,
    curl_off_t /*ulnow*/) {
    auto* cancelled = static_cast<std::atomic<bool>*>(clientp);
    return (cancelled && *cancelled) ? 1 : 0;
}

// ── DuckDuckGo rate limiter ──
// Enforces a minimum gap between successive requests to the free DDG API.
std::chrono::steady_clock::time_point g_last_ddg_request;
std::mutex g_ddg_mutex;

// ── Shared HTTP GET helper (used by web_search and web_fetch) ──
// Returns (body, http_code) or an error string.
Result<std::pair<std::string, long>> http_get(const std::string& url, int timeout_sec,
    std::atomic<bool>* cancelled) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return std::unexpected("curl_easy_init failed");

    std::string body;
    long http_code = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Use system default CA bundle — do NOT set a custom CA path so that
    // curl finds the system trust store automatically.
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return std::unexpected(std::string("web_search curl error: ") +
            curl_easy_strerror(res));
    }
    return std::make_pair(std::move(body), http_code);
}

// ── URL validation helper ──

// Returns true if the scheme is http or https (case-insensitive).
// Rejects file://, ftp://, data:, javascript:, etc.
bool is_valid_fetch_scheme(const std::string& url) {
    auto pos = url.find(':');
    if (pos == std::string::npos) return false;
    std::string scheme = url.substr(0, pos);
    for (auto& c : scheme) c = char(std::tolower((unsigned char)c));
    return scheme == "http" || scheme == "https";
}

// ── Caching for web_fetch ──

std::mutex g_fetch_cache_mutex;
std::unordered_map<std::string, std::string> g_fetch_cache;
