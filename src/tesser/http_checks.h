#pragma once

// The header checks of HttpServer's strict mode (docs/DESIGN.md section
// 10.2), and what it does to text: platform-independent, so they are
// host-tested.

#include <stddef.h>

#include <string>
#include <string_view>

namespace tesser {
namespace http {

// The host of a Host header, without its port: "dev.local:8080" →
// "dev.local", "[fe80::1]:80" → "[fe80::1]".
std::string_view hostName(std::string_view host);
// Whether `origin` ("http://dev.local:8080") is the page of the server a
// request with Host `host` reached: the same host and port, without case, a
// scheme's default port left out of either.
bool sameOrigin(std::string_view origin, std::string_view host);
// Whether a request carrying Origin `origin` may go on: it is `allowed` (the
// CORS origin, without case; "*": any), or the server's own page
// (sameOrigin()). "null" (a sandboxed page, a file) is only allowed by "*".
bool originAllowed(std::string_view origin, const char* allowed, std::string_view host);
// Whether a Content-Type is JSON: "application/json", without case, with or
// without parameters ("; charset=utf-8").
bool isJsonType(std::string_view contentType);
// Percent-decodes in place ('+' means space in query values only), and gives
// the length left. A '%' without two hex digits stays as it is.
size_t urlDecode(char* s, size_t len, bool plusIsSpace);
// attachment; filename="<ASCII>", plus filename*=UTF-8''<percent-encoded>
// (RFC 6266) for other names. Names may come from a file system: quotes,
// backslashes and control bytes would break the header.
std::string contentDisposition(std::string_view name);

}  // namespace http
}  // namespace tesser
