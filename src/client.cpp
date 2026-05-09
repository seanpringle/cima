#include "client.h"

#include <curl/curl.h>
#include <cstring>

ChatClient::ChatClient(std::string api_base, std::string api_key) : api_base_(std::move(api_base)), api_key_(std::move(api_key)) {
  // Strip trailing slash
  while (!api_base_.empty() && api_base_.back() == '/') {
    api_base_.pop_back();
  }
}

ChatClient::~ChatClient() = default;

struct curl_slist* ChatClient::make_headers() const {
  struct curl_slist* list = nullptr;
  list = curl_slist_append(list, "Content-Type: application/json");
  if (!api_key_.empty()) {
    list = curl_slist_append(list, ("Authorization: Bearer " + api_key_).c_str());
  }
  return list;
}

// ---------------------------------------------------------------------------
// curl write callbacks
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Progress callback — abort transfer when g_interrupted is set
// ---------------------------------------------------------------------------

static int progress_cb(void* /*clientp*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) { return g_interrupted ? 1 : 0; }

// ---------------------------------------------------------------------------
// Common curl setup
// ---------------------------------------------------------------------------

static CURL* setup_curl(const std::string& url, struct curl_slist* headers, const std::string& payload_str) {
  CURL* curl = curl_easy_init();
  if (!curl)
    return nullptr;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, payload_str.size());
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "llm-chat/0.1");
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

  return curl;
}

// ---------------------------------------------------------------------------
// Non-streaming chat
// ---------------------------------------------------------------------------

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

  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);

  raw_response_ = body;

  if (res != CURLE_OK) {
    return std::unexpected(std::string("curl error: ") + curl_easy_strerror(res));
  }

  if (http_code != 200) {
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

// ---------------------------------------------------------------------------
// Streaming chat
// ---------------------------------------------------------------------------

Result<void> ChatClient::stream_chat(const json& payload, SSEParser::Callbacks callbacks) {
  std::string payload_str = payload.dump();
  std::string raw;
  long http_code = 0;

  SSEParser parser(std::move(callbacks));

  auto* headers = make_headers();
  CURL* curl = setup_curl(url(), headers, payload_str);
  if (!curl) {
    curl_slist_free_all(headers);
    return std::unexpected(std::string("curl_easy_init failed"));
  }

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &parser);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);

  raw_response_ = parser.raw();

  if (res != CURLE_OK) {
    auto msg = std::string("curl error: ") + curl_easy_strerror(res);
    if (!raw_response_.empty()) {
      msg += " | raw: " + raw_response_.substr(0, 500);
    }
    return std::unexpected(std::move(msg));
  }

  if (http_code != 200) {
    auto msg = "HTTP " + std::to_string(http_code);
    if (!raw_response_.empty()) {
      msg += ": " + raw_response_.substr(0, 500);
    }
    return std::unexpected(msg);
  }

  return {};
}
