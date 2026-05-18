#include "tools.h"

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xmlstring.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    // Use system default CA bundle — do NOT set a custom CA path so that
    // curl finds the system trust store automatically.
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
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

// ===================================================================
// HTTP POST with form-encoded body (for DDG HTML search)
// ===================================================================

Result<std::pair<std::string, long>> http_post_form(const std::string& url,
    const std::string& form_data,
    int timeout_sec,
    std::atomic<bool>* cancelled) {
    CURL* curl = curl_easy_init();
    if (!curl)
        return std::unexpected("curl_easy_init failed");

    std::string body;
    long http_code = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form_data.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(form_data.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, web_search_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cima/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, nullptr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, web_search_progress_cb);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return std::unexpected(std::string("curl error: ") + curl_easy_strerror(res));
    }
    return std::make_pair(std::move(body), http_code);
}

// ===================================================================
// DDG redirect URL extraction
// ===================================================================

std::string extract_uddg_url(const std::string& ddg_url) {
    // Look for "uddg=" in the URL
    auto pos = ddg_url.find("uddg=");
    if (pos == std::string::npos)
        return {};

    pos += 5; // skip past "uddg="

    auto end = ddg_url.find('&', pos);
    if (end == std::string::npos)
        end = ddg_url.size();

    std::string encoded = ddg_url.substr(pos, end - pos);
    if (encoded.empty())
        return {};

    // URL-decode using curl
    int decoded_len = 0;
    char* decoded = curl_easy_unescape(nullptr, encoded.c_str(), static_cast<int>(encoded.size()), &decoded_len);
    if (!decoded)
        return {};

    std::string result(decoded, static_cast<size_t>(decoded_len));
    curl_free(decoded);
    return result;
}

// ===================================================================
// DDG HTML results parser (libxml2)
// ===================================================================

// Forward declaration of the DOM walker helper
static void extract_text_recursive(xmlNode* node, std::string& out);

/// Recursively collect all text content (ignoring tags) from a subtree.
/// This naturally strips <b>, <i>, etc. from snippets.
static void extract_text_recursive(xmlNode* node, std::string& out) {
    for (xmlNode* cur = node; cur; cur = cur->next) {
        if (cur->type == XML_TEXT_NODE) {
            xmlChar* text = xmlNodeGetContent(cur);
            if (text) {
                out += reinterpret_cast<const char*>(text);
                xmlFree(text);
            }
        } else if (cur->type == XML_ELEMENT_NODE) {
            // Recurse into children (skip script/style)
            auto* name = reinterpret_cast<const char*>(cur->name);
            if (name && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0))
                continue;
            extract_text_recursive(cur->children, out);
        }
    }
}

/// Get the value of an attribute by name from an xmlNode.
static std::string get_attr(xmlNode* node, const char* attr_name) {
    for (xmlAttr* attr = node->properties; attr; attr = attr->next) {
        if (xmlStrcmp(attr->name, reinterpret_cast<const xmlChar*>(attr_name)) == 0) {
            xmlChar* val = xmlNodeGetContent(attr->children);
            if (val) {
                std::string s = reinterpret_cast<const char*>(val);
                xmlFree(val);
                return s;
            }
        }
    }
    return {};
}

/// Check if an element's class attribute contains the given substring.
static bool has_class(xmlNode* node, const char* cls) {
    std::string class_attr = get_attr(node, "class");
    return class_attr.find(cls) != std::string::npos;
}

/// Find the first descendant element with a class containing the given string.
static xmlNode* find_descendant_by_class(xmlNode* parent, const char* cls) {
    for (xmlNode* cur = parent->children; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (has_class(cur, cls))
                return cur;
            xmlNode* found = find_descendant_by_class(cur, cls);
            if (found)
                return found;
        }
    }
    return nullptr;
}

// No-op error handler for libxml2 (suppresses stderr chatter).
static void ddg_html_parse_err_func(void* /*ctx*/, const char* /*msg*/, ...) {
    // intentionally empty
}

Result<std::string> ddg_html_parse(const std::string& html) {
    // Suppress libxml2 error messages to stderr
    xmlSetGenericErrorFunc(nullptr, ddg_html_parse_err_func);

    htmlDocPtr doc = htmlReadMemory(
        html.data(), static_cast<int>(html.size()),
        nullptr, "UTF-8",
        HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);

    if (!doc) {
        xmlResetLastError();
        return std::string("(no results found)");
    }

    xmlNode* root = xmlDocGetRootElement(doc);
    if (!root) {
        xmlFreeDoc(doc);
        xmlResetLastError();
        return std::string("(no results found)");
    }

    struct ResultItem {
        std::string title;
        std::string url;
        std::string snippet;
    };
    std::vector<ResultItem> items;

    // Walk the DOM looking for <div class="result ... web-result ...">
    // We do a recursive traversal.
    std::function<void(xmlNode*)> walk = [&](xmlNode* node) {
        for (xmlNode* cur = node; cur; cur = cur->next) {
            if (cur->type == XML_ELEMENT_NODE &&
                reinterpret_cast<const char*>(cur->name) &&
                strcmp(reinterpret_cast<const char*>(cur->name), "div") == 0 &&
                has_class(cur, "web-result"))
            {
                // Found a result div. Extract title, snippet, URL.
                ResultItem item;

                // Title: <a class="result__a">
                xmlNode* title_a = find_descendant_by_class(cur, "result__a");
                if (title_a) {
                    extract_text_recursive(title_a->children, item.title);
                    // Trim whitespace
                    while (!item.title.empty() && (item.title.back() == ' ' || item.title.back() == '\n' || item.title.back() == '\r' || item.title.back() == '\t'))
                        item.title.pop_back();
                    // Extract href for URL
                    std::string href = get_attr(title_a, "href");
                    if (!href.empty()) {
                        item.url = extract_uddg_url(href);
                    }
                }

                // Snippet: <a class="result__snippet">
                xmlNode* snippet_a = find_descendant_by_class(cur, "result__snippet");
                if (snippet_a) {
                    extract_text_recursive(snippet_a->children, item.snippet);
                    while (!item.snippet.empty() && (item.snippet.back() == ' ' || item.snippet.back() == '\n' || item.snippet.back() == '\r' || item.snippet.back() == '\t'))
                        item.snippet.pop_back();
                }

                if (!item.title.empty()) {
                    items.push_back(std::move(item));
                }
            }

            // Recurse into children
            if (cur->children)
                walk(cur->children);
        }
    };

    walk(root);
    xmlFreeDoc(doc);
    xmlResetLastError();

    if (items.empty())
        return std::string("(no results found)");

    std::string result;
    int rank = 1;
    for (const auto& item : items) {
        if (rank > 10)
            break;
        result += std::to_string(rank) + ". " + item.title + "\n";
        if (!item.snippet.empty())
            result += "   " + item.snippet + "\n";
        if (!item.url.empty())
            result += "   " + item.url + "\n";
        result += "\n";
        rank++;
    }

    return result;
}
