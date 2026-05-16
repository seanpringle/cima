#include "tools.h"

#include <curl/curl.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// ===================================================================
// web_search — single backend: DuckDuckGo HTML interface
// ===================================================================

Tool make_web_search_tool(int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web using DuckDuckGo. "
        "Returns up to 10 results with titles, snippets, and URLs. "
        "No API key required.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"query",
                {{"type", "string"}, {"description", "Search query (max 500 characters)"}}}}},
        {"required", {"query"}}};
    t.execute = [cancelled, timeout](const json& args) -> Result<std::string> {
        auto query = args.value("query", std::string());
        if (query.empty())
            return std::unexpected("query is required");

        if (query.size() > 500)
            query = query.substr(0, 500);

        // ── Rate-limit with jitter ──
        // DDG requests must be spaced at least ~1s apart; add random jitter
        // to avoid accidental DoS.
        {
            std::lock_guard<std::mutex> lock(g_ddg_mutex);
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - g_last_ddg_request);

            // Jitter: random 0..500ms added to base 1000ms
            auto jitter = std::chrono::milliseconds(std::rand() % DDG_JITTER_MAX.count());
            auto total_delay = DDG_MIN_INTERVAL + jitter;

            if (elapsed < total_delay) {
                std::this_thread::sleep_for(total_delay - elapsed);
            }
            g_last_ddg_request = std::chrono::steady_clock::now();
        }

        // ── Build form data ──
        char* enc_q = curl_easy_escape(nullptr, query.c_str(), static_cast<int>(query.size()));
        if (!enc_q)
            return std::unexpected("curl_easy_escape failed");
        std::string form_data = std::string("q=") + enc_q;
        curl_free(enc_q);

        // ── Retry loop with exponential backoff on HTTP 429 ──
        std::string body;
        long http_code = 0;
        int max_retries = 3;
        int delay_ms = 1000;

        for (int attempt = 0; attempt <= max_retries; attempt++) {
            auto resp = http_post_form(
                "https://html.duckduckgo.com/html/", form_data,
                timeout, cancelled.get());
            if (!resp) {
                if (attempt == max_retries)
                    return std::unexpected(resp.error());
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms *= 2;
                continue;
            }
            body = resp->first;
            http_code = resp->second;
            if (http_code != 429)
                break;
            if (attempt < max_retries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                delay_ms *= 2;
            }
        }

        if (http_code != 200) {
            std::string msg = "web_search HTTP " + std::to_string(http_code);
            if (!body.empty())
                msg += ": " + body.substr(0, 500);
            return std::unexpected(msg);
        }

        // ── Parse HTML results ──
        return ddg_html_parse(body);
    };
    return t;
}

Tool make_web_fetch_tool(int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch the content of a URL. Returns the response body as text. "
        "Max 100,000 characters. "
        "Use this to read documentation, API references, or web pages. "
        "Only returns the raw response body; for search results use web_search.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"url",
                {{"type", "string"},
                    {"description",
                        "URL to fetch (http/https). "
                        "Results are cached per session — re-fetching the same URL "
                        "returns the cached content."}}}}},
        {"required", {"url"}}};
    t.execute = [cancelled](const json& args) -> Result<std::string> {
        auto url = args.value("url", std::string());
        if (url.empty()) {
            return std::unexpected("url is required");
        }

        // ── URL validation ──
        // 1. Check scheme (http/https only)
        if (!is_valid_fetch_scheme(url)) {
            return std::unexpected("web_fetch: only http and https URLs are supported");
        }

        // ── Cache check ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            auto it = g_fetch_cache.find(url);
            if (it != g_fetch_cache.end()) {
                return it->second;
            }
        }

        // ── HTTP GET via libcurl ──
        CURL* curl = curl_easy_init();
        if (!curl) {
            return std::unexpected("web_fetch: curl_easy_init failed");
        }

        std::string body;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/0.1");
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled.get());

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        // Read Content-Type BEFORE curl_easy_cleanup — the pointer is only
        // valid while the handle lives.
        std::string content_type;
        char* ct_raw = nullptr;
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct_raw);
        if (ct_raw != nullptr) {
            content_type = ct_raw;
        }

        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_fetch curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_fetch HTTP " + std::to_string(http_code);
            if (!body.empty()) {
                msg += ": " + body.substr(0, 500);
            }
            return std::unexpected(msg);
        }

        // ── Content-Type filtering ──
        // Only allow text-based content types. If the server didn't send a
        // Content-Type header, we allow it (assume text).
        if (!content_type.empty()) {
            std::string ct = content_type;
            // Convert to lowercase for comparison
            for (auto& c : ct) c = char(std::tolower((unsigned char)c));

            // Strip parameters like charset=utf-8
            auto semi = ct.find(';');
            if (semi != std::string::npos) ct = ct.substr(0, semi);

            // Trim trailing whitespace
            while (!ct.empty() && (ct.back() == ' ' || ct.back() == '\t'))
                ct.pop_back();

            // Allowed content types
            bool allowed = false;
            if (ct.find("text/") == 0) allowed = true;
            else if (ct == "application/json") allowed = true;
            else if (ct == "application/xml") allowed = true;
            else if (ct == "application/javascript") allowed = true;
            else if (ct == "application/x-javascript") allowed = true;
            else if (ct == "application/atom+xml") allowed = true;
            else if (ct == "application/rss+xml") allowed = true;
            else if (ct == "application/xhtml+xml") allowed = true;

            if (!allowed) {
                return std::unexpected(
                    "web_fetch: unsupported Content-Type '" + ct +
                    "' — only text-based content can be fetched");
            }
        }

        // ── Truncation ──
        const size_t max_chars = 100000;
        if (body.size() > max_chars) {
            body = body.substr(0, max_chars) +
                "\n...(truncated, >" + std::to_string(max_chars) + " chars)";
        }

        // ── Cache the result ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            g_fetch_cache[url] = body;
        }

        return body;
    };
    return t;
}
