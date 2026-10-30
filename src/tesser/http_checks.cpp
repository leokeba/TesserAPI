#include "tesser/http_checks.h"

#include <ctype.h>
#include <string.h>

namespace tesser {
namespace http {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// Where the port starts in host[:port] (an IPv6 literal is bracketed), or
// npos.
size_t portAt(std::string_view host) {
    size_t colon = host.rfind(':');
    if (colon == std::string_view::npos) return colon;
    size_t bracket = host.rfind(']');
    if (bracket != std::string_view::npos && bracket > colon) return std::string_view::npos;
    return colon;
}

// host[:port] without a default port.
std::string_view withoutDefaultPort(std::string_view authority, std::string_view port) {
    size_t at = portAt(authority);
    if (at != std::string_view::npos && authority.substr(at + 1) == port) return authority.substr(0, at);
    return authority;
}

}  // namespace

std::string_view hostName(std::string_view host) {
    size_t at = portAt(host);
    return at == std::string_view::npos ? host : host.substr(0, at);
}

bool sameOrigin(std::string_view origin, std::string_view host) {
    size_t sep = origin.find("://");
    if (sep == std::string_view::npos || host.empty()) return false;
    std::string_view scheme = origin.substr(0, sep);
    std::string_view authority = origin.substr(sep + 3);
    if (authority.empty() || authority.find('/') != std::string_view::npos) return false;
    std::string_view port = equalsNoCase(scheme, "https") ? "443" : equalsNoCase(scheme, "http") ? "80" : "";
    if (port.empty()) return false;
    return equalsNoCase(withoutDefaultPort(authority, port), withoutDefaultPort(host, port));
}

bool originAllowed(std::string_view origin, const char* allowed, std::string_view host) {
    if (allowed && strcmp(allowed, "*") == 0) return true;
    if (allowed && equalsNoCase(origin, allowed)) return true;
    return sameOrigin(origin, host);
}

bool isJsonType(std::string_view contentType) {
    size_t end = contentType.find(';');
    std::string_view type = contentType.substr(0, end);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) type.remove_suffix(1);
    while (!type.empty() && (type.front() == ' ' || type.front() == '\t')) type.remove_prefix(1);
    return equalsNoCase(type, "application/json");
}

namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

size_t urlDecode(char* s, size_t len, bool plusIsSpace) {
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '%' && i + 2 < len && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            c = static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            c = ' ';
        }
        s[out++] = c;
    }
    return out;
}

std::string contentDisposition(std::string_view name) {
    std::string d = "attachment; filename=\"";
    bool ascii = true;
    for (char c : name) {
        auto u = static_cast<unsigned char>(c);
        if (u >= 0x80) ascii = false;
        d += (u < 0x20 || u >= 0x7f || c == '"' || c == '\\') ? '_' : c;
    }
    d += '"';
    if (!ascii) {
        static const char kHex[] = "0123456789ABCDEF";
        d += "; filename*=UTF-8''";
        for (char c : name) {
            auto u = static_cast<unsigned char>(c);
            if (isalnum(u) || (c && strchr("!#$&+-.^_`|~", c))) {
                d += c;
            } else {
                d += '%';
                d += kHex[u >> 4];
                d += kHex[u & 15];
            }
        }
    }
    return d;
}

}  // namespace http
}  // namespace tesser
