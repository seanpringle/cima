#include "tools.h"

#include <curl/curl.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

Tool make_web_search_tool(const std::string& api_key,
    const std::string& engine_id,
    const std::string& endpoint_override,
    int timeout,
    CancellationToken cancelled) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web. Returns up to 10 results with titles, snippets, and URLs. "
        "By default uses the DuckDuckGo Instant Answer API (no key required). "
        "To use Google Custom Search, set SEARCH_API_KEY + SEARCH_ENGINE_ID. "
        "For a custom endpoint, set SEARCH_ENDPOINT with a {query} placeholder.";
    t.timeout_sec = timeout;
    t.parameters = {{"type", "object"},
        {"properties",
            {{"query",
                {{"type", "string"}, {"description", "Search query (max 500 characters)"}}}}},
        {"required", {"query"}}};
    t.execute = [api_key, engine_id, endpoint_override, cancelled](
                    const json& args) -> Result<std::string> {
        auto query = args.value("query", std::string());
        if (query.empty())
            return std::unexpected("query is required");

        if (query.size() > 500)
            query = query.substr(0, 500);

        // Determine which backend to use
        bool use_google = !api_key.empty() && !engine_id.empty();
        bool use_custom = !endpoint_override.empty();
        bool use_ddg = !use_google && !use_custom;

        // Build the request URL
        std::string url;
        if (use_custom) {
            url = endpoint_override;
            auto pos = url.find("{query}");
            if (pos != std::string::npos) {
                char* encoded = curl_easy_escape(nullptr, query.c_str(), (int)query.size());
                if (!encoded)
                    return std::unexpected("curl_easy_escape failed");
                url.replace(pos, 7, encoded);
                curl_free(encoded);
            }
        } else if (use_google) {
            char* enc_key = curl_easy_escape(nullptr, api_key.c_str(), 0);
            char* enc_cx = curl_easy_escape(nullptr, engine_id.c_str(), 0);
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_key || !enc_cx || !enc_q) {
                curl_free(enc_key);
                curl_free(enc_cx);
                curl_free(enc_q);
                return std::unexpected("curl_easy_escape failed");
            }
            url = "https://www.googleapis.com/customsearch/v1?key=" +
                std::string(enc_key) + "&cx=" + std::string(enc_cx) + "&q=" + std::string(enc_q);
            curl_free(enc_key);
            curl_free(enc_cx);
            curl_free(enc_q);
        } else {
            // DuckDuckGo Instant Answer API — no API key required
            char* enc_q = curl_easy_escape(nullptr, query.c_str(), 0);
            if (!enc_q)
                return std::unexpected("curl_easy_escape failed");
            url = "https://api.duckduckgo.com/?q=" +
                std::string(enc_q) + "&format=json&no_html=1&skip_disambig=1";
            curl_free(enc_q);

            // Rate-limit: DDG free API requires ~1s between requests
            {
                std::lock_guard<std::mutex> lock(g_ddg_mutex);
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - g_last_ddg_request);
                if (elapsed < DDG_MIN_INTERVAL) {
                    std::this_thread::sleep_for(DDG_MIN_INTERVAL - elapsed);
                }
                g_last_ddg_request = std::chrono::steady_clock::now();
            }

            // Retry loop with exponential backoff on HTTP 429
            std::string body;
            long http_code = 0;
            int max_retries = 3;
            int delay_ms = 1000;
            for (int attempt = 0; attempt <= max_retries; attempt++) {
                auto resp = http_get(url, 15, cancelled.get());
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

            // Parse JSON response
            json j;
            try {
                j = json::parse(body);
            } catch (const json::parse_error& e) {
                return std::unexpected("web_search JSON parse error: " + std::string(e.what()));
            }

            // Format DuckDuckGo Instant Answer response
            // See: https://duckduckgo.com/api
            {
                std::string result;

                // Direct answer (e.g. "12 months" for "months in a year")
                if (!j.value("Answer", "").empty()) {
                    result += "Answer: " + j["Answer"].get<std::string>() + "\n\n";
                }

                // Abstract/summary
                std::string abstract = j.value("AbstractText", "");
                if (!abstract.empty()) {
                    result += abstract + "\n";
                    std::string src = j.value("AbstractSource", "");
                    std::string src_url = j.value("AbstractURL", "");
                    if (!src.empty())
                        result += "Source: " + src + " (" + src_url + ")\n";
                    result += "\n";
                }

                // Definition
                std::string def = j.value("Definition", "");
                if (!def.empty()) {
                    result += "Definition: " + def + "\n\n";
                }

                // Heading (the topic of the instant answer)
                std::string heading = j.value("Heading", "");
                if (!heading.empty() && abstract.empty()) {
                    result += "Topic: " + heading + "\n\n";
                }

                // Results array (top 5)
                if (j.contains("Results") && j["Results"].is_array()) {
                    for (const auto& item : j["Results"]) {
                        std::string text = item.value("Text", "");
                        std::string link = item.value("FirstURL", "");
                        if (!text.empty()) {
                            result += "- " + text + "\n";
                            if (!link.empty()) result += "  " + link + "\n";
                            result += "\n";
                        }
                    }
                }

                // RelatedTopics (flattening subcategories)
                if (j.contains("RelatedTopics") && j["RelatedTopics"].is_array()) {
                    for (const auto& topic : j["RelatedTopics"]) {
                        if (topic.contains("Topics") && topic["Topics"].is_array()) {
                            // Nested subcategory
                            for (const auto& sub : topic["Topics"]) {
                                std::string text = sub.value("Text", "");
                                std::string link = sub.value("FirstURL", "");
                                if (!text.empty()) {
                                    result += "- " + text + "\n";
                                    if (!link.empty()) result += "  " + link + "\n";
                                    result += "\n";
                                }
                            }
                        } else {
                            std::string text = topic.value("Text", "");
                            std::string link = topic.value("FirstURL", "");
                            if (!text.empty()) {
                                result += "- " + text + "\n";
                                if (!link.empty()) result += "  " + link + "\n";
                                result += "\n";
                            }
                        }
                    }
                }

                if (result.empty())
                    return std::string("(no results found)");
                return result;
            }
        }

        // =====================================================================
        // Google CSE and custom endpoint share the same HTTP + parsing logic
        // =====================================================================

        // libcurl GET
        CURL* curl = curl_easy_init();
        if (!curl)
            return std::unexpected("curl_easy_init failed");

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
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled.get());

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return std::unexpected(std::string("web_search curl error: ") +
                curl_easy_strerror(res));
        }

        if (http_code != 200) {
            std::string msg = "web_search HTTP " + std::to_string(http_code);
            if (!body.empty())
                msg += ": " + body.substr(0, 500);
            return std::unexpected(msg);
        }

        // Parse JSON response
        json j;
        try {
            j = json::parse(body);
        } catch (const json::parse_error& e) {
            return std::unexpected("web_search JSON parse error: " + std::string(e.what()));
        }

        // Google CSE or custom endpoint: expects {"items": [...]}
        if (!j.contains("items") || !j["items"].is_array() || j["items"].empty()) {
            return std::string("(no results found)");
        }
        std::string result;
        int rank = 1;
        for (const auto& item : j["items"]) {
            if (rank > 10)
                break;
            std::string title = item.value("title", "(no title)");
            std::string snippet = item.value("snippet", "");
            std::string link = item.value("link", "");
            result += std::to_string(rank) + ". " + title + "\n";
            if (!snippet.empty())
                result += "   " + snippet + "\n";
            if (!link.empty())
                result += "   " + link + "\n";
            result += "\n";
            rank++;
        }
        return result;
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
