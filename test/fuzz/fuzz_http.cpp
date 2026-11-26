// What an HTTP client controls before a request reaches the core, in the
// platform-independent half of HttpServer (tesser/http_checks.h): the
// percent-decoding of the path and of query values (?filename=, ?keys=,
// ?shape=...), the Content-Disposition built from a file name, and the
// strict mode's Host and Origin checks. The query parser itself is
// ESP-IDF's (httpd_query_key_value), not built on the host.
//
// The input is split at its first two NUL bytes into three fields, used as
// a query value, a Host header and an Origin header.
#include "fuzz_common.h"

#include <string.h>

#include "tesser/http_checks.h"

namespace {

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

void decode(std::string_view in, bool plusIsSpace) {
    std::vector<char> buf(in.begin(), in.end());
    size_t n = tesser::http::urlDecode(buf.data(), buf.size(), plusIsSpace);
    if (n > in.size()) fuzz::fail("urlDecode grew its input", std::string(in));
    // Each escape takes three bytes to one: the length tells how many.
    size_t escapes = 0;
    for (size_t i = 0; i + 2 < in.size(); i++) {
        if (in[i] == '%' && isHex(in[i + 1]) && isHex(in[i + 2])) {
            escapes++;
            i += 2;
        }
    }
    if (n != in.size() - 2 * escapes) fuzz::fail("urlDecode length", std::string(in));
}

void disposition(std::string_view name) {
    std::string d = tesser::http::contentDisposition(name);
    // A header value: no control bytes, no line breaks, nothing past ASCII.
    for (char c : d) {
        auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7f) fuzz::fail("Content-Disposition carries a raw byte", d);
    }
    // The quoted name ends at the first unescaped quote: the one closing it.
    size_t open = d.find('"');
    size_t close = d.find('"', open + 1);
    if (open == std::string::npos || close == std::string::npos || d.find('"', close + 1) != std::string::npos)
        fuzz::fail("Content-Disposition quoting", d);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view all(reinterpret_cast<const char*>(data), size);
    size_t a = all.find('\0');
    std::string_view value = all.substr(0, a);
    std::string_view rest = a == std::string_view::npos ? std::string_view() : all.substr(a + 1);
    size_t b = rest.find('\0');
    std::string_view host = rest.substr(0, b);
    std::string_view origin = b == std::string_view::npos ? std::string_view() : rest.substr(b + 1);

    decode(value, true);
    decode(value, false);
    disposition(value);
    std::string_view name = tesser::http::hostName(host);
    if (name.size() > host.size()) fuzz::fail("hostName grew", std::string(host));
    std::string allowed(origin);  // the CORS origin is a C string
    bool same = tesser::http::sameOrigin(origin, host);
    bool any = tesser::http::originAllowed(origin, "*", host);
    bool own = tesser::http::originAllowed(origin, nullptr, host);
    (void)tesser::http::originAllowed(origin, allowed.c_str(), host);
    (void)tesser::http::isJsonType(value);
    if (same && !own) fuzz::fail("own origin refused", std::string(origin) + " / " + std::string(host));
    if (!any) fuzz::fail("'*' refused an origin", std::string(origin));
    return 0;
}
