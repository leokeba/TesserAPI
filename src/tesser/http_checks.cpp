#include "tesser/http_checks.h"

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

}  // namespace http
}  // namespace tesser
