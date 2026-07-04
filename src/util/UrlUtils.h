#pragma once
#include <string>

namespace UrlUtils {

/**
 * Check if URL uses HTTPS protocol
 */
bool isHttpsUrl(const std::string& url);

/**
 * Prepend http:// if no protocol specified (server will redirect to https if needed)
 */
std::string ensureProtocol(const std::string& url);

/**
 * Extract host with protocol from URL (e.g., "http://example.com" from "http://example.com/path")
 */
std::string extractHost(const std::string& url);

/**
 * Build full URL from server URL and path.
 * If path starts with /, it's an absolute path from the host root.
 * Otherwise, it's relative to the server URL.
 */
std::string buildUrl(const std::string& serverUrl, const std::string& path);

/**
 * RFC 3986 percent-encode for URL components (unreserved-safe: A-Z / a-z /
 * 0-9 / `-` / `_` / `.` / `~` pass through unchanged; everything else becomes
 * `%XX`).
 *
 * Callers MUST still escape structural characters (`&`, `=`, `#`, `?`) manually
 * when building query strings.
 */
std::string urlencode(const std::string& s);

}  // namespace UrlUtils
