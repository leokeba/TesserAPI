#pragma once

// The header checks of HttpServer's strict mode (docs/DESIGN.md section
// 10.2): platform-independent, so they are host-tested.

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

}  // namespace http
}  // namespace tesser
