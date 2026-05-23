#include "tools.h"

#include <curl/curl.h>
#include <html2md.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

using namespace std::chrono_literals;

// ===================================================================
// web_search — single backend: DuckDuckGo HTML interface
// ===================================================================

Tool make_web_search_tool(const Config& config, int timeout, CancellationToken cancelled) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web using DuckDuckGo. "
        "Returns up to 10 results with titles, snippets, and URLs. "
        "No API key required. "
        "DuckDuckGo aggressively rate-limits requests; at least 3 seconds "
        "must elapse between successive calls. If you need multiple searches, "
        "space them out — parallel calls are serialized with enforced delays. " + config.TOOL_LOG_NOTE;
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

        // ── Build GET URL (no shared state, safe outside lock) ──
        char* enc_q = curl_easy_escape(nullptr, query.c_str(), static_cast<int>(query.size()));
        if (!enc_q)
            return std::unexpected("curl_easy_escape failed");
        std::string url = std::string("https://html.duckduckgo.com/html/?q=") + enc_q;
        curl_free(enc_q);

        std::lock_guard<std::mutex> lock(g_ddg_mutex);
        // Ensure we can't hammer DDG
        std::this_thread::sleep_for(3s);

        // 2. Execute HTTP GET request (POST triggers DDG challenge page)
        auto resp = http_get(url, timeout, cancelled.get());
        std::string body = resp->first;
        long http_code = resp->second;

        if (http_code != 200) {
            std::string msg = "web_search HTTP " + std::to_string(http_code);
            // Detect DDG challenge/CAPTCHA page (HTTP 202)
            if (http_code == 202) {
                msg = "DuckDuckGo returned a challenge page (HTTP 202). "
                      "This usually means your IP is being rate-limited. "
                      "Wait a moment and try again, or use a different network.";
            } else if (!body.empty()) {
                msg += ": " + body.substr(0, 500);
            }
            return std::unexpected(msg);
        }

        return ddg_html_parse(body);
    };
    return t;
}

Tool make_web_fetch_tool(const Config& config, int timeout, CancellationToken cancelled,
    std::shared_ptr<std::vector<std::string>> tool_logs) {
    Tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch the content of a URL. Returns the response body as text. "
        "HTML pages are automatically converted to clean Markdown. "
        "Use this to read documentation, API references, or web pages. "
        "For search results use web_search. " + config.TOOL_LOG_NOTE;
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
    t.execute = [cancelled, tool_logs](const json& args) -> Result<std::string> {
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
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/1.0");
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

        // ── HTML → Markdown conversion ──
        // If the response is HTML, convert it to Markdown for easier reading.
        bool is_html = false;
        if (!content_type.empty()) {
            std::string ct_lower = content_type;
            for (auto& c : ct_lower)
                c = char(std::tolower((unsigned char)c));
            auto semi = ct_lower.find(';');
            if (semi != std::string::npos)
                ct_lower = ct_lower.substr(0, semi);
            while (!ct_lower.empty() &&
                   (ct_lower.back() == ' ' || ct_lower.back() == '\t'))
                ct_lower.pop_back();
            is_html = (ct_lower == "text/html" ||
                       ct_lower == "application/xhtml+xml");
        }
        // Heuristic: if Content-Type is missing or text/* but body looks like
        // HTML, convert it anyway (handles misconfigured servers).
        if (!is_html) {
            auto start = body.find_first_not_of(" \t\r\n");
            if (start != std::string::npos) {
                // Check first non-whitespace character is '<' and there's a
                // known HTML tag or DOCTYPE declaration early in the body.
                std::size_t remaining = body.size() - start;
                is_html =
                    (remaining >= 7 && body.compare(start, 7, "<!DOCTYPE") == 0) ||
                    (remaining >= 6 && body.compare(start, 6, "<html ") == 0) ||
                    (remaining >= 6 && body.compare(start, 6, "<html>") == 0);
            }
        }

        if (is_html) {
            body = html2md::Convert(body);
        }

        // ── Cache the result (before potential move into tool_logs) ──
        {
            std::lock_guard<std::mutex> lock(g_fetch_cache_mutex);
            g_fetch_cache[url] = body;
        }

        // ── Spill to tool_logs if output exceeds threshold ──
        return spill_long_output(std::move(body), tool_logs);
    };
    return t;
}
