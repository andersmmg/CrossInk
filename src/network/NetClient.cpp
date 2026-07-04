#include "NetClient.h"

#include <Logging.h>

#include <cstdio>
#include <string>

#ifdef SIMULATOR
#include <curl/curl.h>
#else
#include "HttpDownloader.h"
#endif

namespace {
#ifdef SIMULATOR
size_t curlWriteString(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const size_t bytes = size * nmemb;
  out->append(ptr, bytes);
  return bytes;
}
#endif
}  // namespace

namespace NetClient {

std::string g_lastError;

#ifdef SIMULATOR
bool get(const std::string& url, std::string& outBody) {
  outBody.clear();
  g_lastError.clear();

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    g_lastError = "curl_easy_init failed";
    LOG_ERR("NET", "NetClient::get(%s) %s", url.c_str(), g_lastError.c_str());
    return false;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "CrossInk-LuaBridge/0.1 (simulator)");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);

  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "curl: %s", curl_easy_strerror(rc));
    g_lastError = buf;
    LOG_ERR("NET", "NetClient::get(%s) failed: %s", url.c_str(), g_lastError.c_str());
    return false;
  }
  if (status < 200 || status >= 300) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
    g_lastError = buf;
    LOG_ERR("NET", "NetClient::get(%s) %s, %zu bytes", url.c_str(), g_lastError.c_str(), outBody.size());
    return false;
  }

  LOG_DBG("NET", "NetClient::get(%s) HTTP %ld, %zu bytes", url.c_str(), status, outBody.size());
  return true;
}
#else
bool get(const std::string& url, std::string& outBody) {
  outBody.clear();
  g_lastError.clear();

  // HttpDownloader::fetchUrl parses the URL itself, supports redirected
  // HTTPS, and uses the ESP-IDF x509 bundle for trust. Single static call —
  // no extra wiring required.
  if (!HttpDownloader::fetchUrl(url, outBody)) {
    g_lastError = "HttpDownloader::fetchUrl returned false";
    LOG_ERR("NET", "NetClient::get(%s) failed via HttpDownloader", url.c_str());
    return false;
  }
  LOG_DBG("NET", "NetClient::get(%s) OK %zu bytes", url.c_str(), outBody.size());
  return true;
}
#endif

const std::string& lastError() { return g_lastError; }

}  // namespace NetClient
