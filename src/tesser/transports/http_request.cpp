#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

#include <stdlib.h>
#include <string.h>

#include "lwip/sockets.h"

// Reading a request: its path and query, its token and headers, the strict
// mode (docs/DESIGN.md section 10.2).

namespace tesser {
namespace http {

int recvBody(httpd_req_t* req, char* buf, size_t len) {
    for (int timeouts = 0;;) {
        int n = httpd_req_recv(req, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT || ++timeouts >= 4) return n;
    }
}

bool drain(httpd_req_t* req, size_t left) {
    char buf[256];
    while (left) {
        int n = recvBody(req, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n <= 0) return false;
        left -= static_cast<size_t>(n);
    }
    return true;
}

bool sendAll(httpd_req_t* req, const char* data, size_t len) {
    while (len) {
        int n = httpd_send(req, data, len);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

const char* statusLine(Status s) {
    switch (httpCode(s)) {
        case 200: return "200 OK";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 404: return "404 Not Found";
        case 405: return "405 Method Not Allowed";
        case 413: return "413 Payload Too Large";
        case 422: return "422 Unprocessable Entity";
        case 503: return "503 Service Unavailable";
        case 504: return "504 Gateway Timeout";
        default: return "500 Internal Server Error";
    }
}

namespace {

// Reads one query parameter into buf (decoded). Returns false when absent;
// sets `bad` when present but too long.
bool queryParam(const char* query, const char* key, char* buf, size_t cap, size_t& len, bool& bad) {
    if (!query) return false;
    esp_err_t err = httpd_query_key_value(query, key, buf, cap);
    if (err == ESP_ERR_NOT_FOUND) return false;
    if (err != ESP_OK) {
        bad = true;
        return false;
    }
    len = urlDecode(buf, strlen(buf), true);
    buf[len] = '\0';
    return true;
}

// A boolean query parameter: true/false/1/0. Leaves `out` alone when absent.
void boolParam(const char* query, const char* key, bool& out, bool& bad) {
    char buf[8];
    size_t len = 0;
    if (!queryParam(query, key, buf, sizeof(buf), len, bad)) return;
    std::string_view v(buf, len);
    if (v == "true" || v == "1") {
        out = true;
    } else if (v == "false" || v == "0") {
        out = false;
    } else {
        bad = true;
    }
}

// A flag that may come bare (?sub) or with a boolean value (?sub=1).
bool flagParam(const char* query, const char* key, bool& bad) {
    size_t keyLen = strlen(key);
    for (const char* p = query; p && *p;) {
        const char* amp = strchr(p, '&');
        size_t n = amp ? static_cast<size_t>(amp - p) : strlen(p);
        if (n == keyLen && memcmp(p, key, keyLen) == 0) return true;
        p = amp ? amp + 1 : nullptr;
    }
    char buf[8];
    size_t len = 0;
    if (!queryParam(query, key, buf, sizeof(buf), len, bad)) return false;
    if (len == 0) return true;  // ?sub=
    bool on = false;
    boolParam(query, key, on, bad);
    return on;
}

// A text parameter into `out`, decoded (of `room` bytes at most, its end
// included); empty when absent.
bool textParam(const char* query, const char* key, size_t room, std::string& out, bool& bad) {
    size_t len = 0;
    out.resize(room);
    bool found = queryParam(query, key, &out[0], room, len, bad);
    out.resize(found ? len : 0);
    return found;
}

constexpr size_t kMaxToken = 256;

}  // namespace

bool readQuery(httpd_req_t* req, Request& request, QueryText& text, Reply& reply, std::string_view path) {
    char* query = nullptr;
    size_t queryLen = httpd_req_get_url_query_len(req);
    char value[16];
    bool bad = false;
    if (queryLen) {
        query = static_cast<char*>(malloc(queryLen + 1));
        if (!query || httpd_req_get_url_query_str(req, query, queryLen + 1) != ESP_OK) {
            free(query);
            writeError(reply, Status::Internal, Reason::LowMemory, path, "out of memory");
            return false;
        }
        size_t len = 0;
        if (queryParam(query, "depth", value, sizeof(value), len, bad)) {
            char* end = nullptr;
            long d = strtol(value, &end, 10);
            if (len == 0 || *end || d < 0 || d > 255) bad = true;
            request.query.depth = static_cast<int>(d);
        }
        if (textParam(query, "keys", queryLen + 1, text.keys, bad)) request.query.keys = text.keys;
        if (textParam(query, "exclude", queryLen + 1, text.exclude, bad)) request.query.exclude = text.exclude;
        if (queryParam(query, "view", value, sizeof(value), len, bad) &&
            !parseView(std::string_view(value, len), request.query.view)) {
            bad = true;
        }
        // A streamed subscription (?sub, sub=1 or sub=true) and its options.
        text.sub = flagParam(query, "sub", bad);
        if (queryParam(query, "interval", value, sizeof(value), len, bad)) {
            char* end = nullptr;
            unsigned long ms = strtoul(value, &end, 10);
            if (len == 0 || *end || value[0] == '-') bad = true;
            request.query.interval = static_cast<uint32_t>(ms);
        }
        boolParam(query, "changes", request.query.changes, bad);
        // events=false, or events=<a ui hint>: only the events that have it.
        if (textParam(query, "events", queryLen + 1, text.eventHint, bad)) {
            if (text.eventHint == "true" || text.eventHint == "1") {
                text.eventHint.clear();
            } else if (text.eventHint == "false" || text.eventHint == "0") {
                request.query.events = false;
                text.eventHint.clear();
            } else if (text.eventHint.empty()) {
                bad = true;
            } else {
                request.query.eventHint = text.eventHint;
            }
        }
        boolParam(query, "snapshot", request.query.snapshot, bad);
        boolParam(query, "remotes", request.query.remotes, bad);
        // Browsers can't send a GET body, so a shape may also come as ?shape=<json>.
        textParam(query, "shape", queryLen + 1, text.shape, bad);
        // The file name the client gave, for a transfer (FileRequest::filename).
        textParam(query, "filename", queryLen + 1, text.filename, bad);
        free(query);
        if (bad) {
            writeError(reply, Status::BadRequest, path, "invalid query parameter");
            return false;
        }
    }
    return true;
}

std::string requestToken(httpd_req_t* req) {
    std::string token;
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len > 7 && len <= 7 + kMaxToken) {
        std::string value(len + 1, '\0');
        if (httpd_req_get_hdr_value_str(req, "Authorization", &value[0], value.size()) == ESP_OK &&
            memcmp(value.data(), "Bearer ", 7) == 0) {
            token.assign(value.data() + 7, len - 7);
            return token;
        }
    }
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen) {
        std::string query(qlen + 1, '\0');
        std::string value(kMaxToken + 2, '\0');
        if (httpd_req_get_url_query_str(req, &query[0], query.size()) == ESP_OK &&
            httpd_query_key_value(query.c_str(), "token", &value[0], value.size()) == ESP_OK) {
            size_t n = urlDecode(&value[0], strlen(value.c_str()), true);
            token.assign(value.data(), n);
        }
    }
    return token;
}

std::string header(httpd_req_t* req, const char* name) {
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (!len) return std::string();
    std::string value(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, &value[0], value.size()) != ESP_OK) return std::string();
    value.resize(len);
    return value;
}

void refuse(httpd_req_t* req, const char* statusLine, Status status, Reason reason, const char* message,
            const char* corsOrigin) {
    StringReply body;
    writeError(body, status, reason, std::string_view(), message);
    httpd_resp_set_status(req, statusLine);
    httpd_resp_set_type(req, "application/json");
    if (corsOrigin) httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", corsOrigin);
    httpd_resp_send(req, body.body.data(), static_cast<ssize_t>(body.body.size()));
}

void peerAddress(int fd, Client& client) {
    struct sockaddr_storage addr = {};
    socklen_t addrLen = sizeof(addr);
    if (fd < 0 || getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) != 0) return;
    if (addr.ss_family == AF_INET) {
        memcpy(client.address, &reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr, 4);
        client.addressLength = 4;
    }
#if LWIP_IPV6
    else if (addr.ss_family == AF_INET6) {
        memcpy(client.address, &reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_addr, 16);
        client.addressLength = 16;
    }
#endif
}

}  // namespace http

void HttpServer::strict(StrictOptions options) {
    auto shared = std::make_shared<const StrictOptions>(std::move(options));
    MutexGuard guard(strictMutex_);
    strict_ = std::move(shared);
}

void HttpServer::strict(std::nullptr_t) {
    std::shared_ptr<const StrictOptions> old;
    MutexGuard guard(strictMutex_);
    old.swap(strict_);  // freed after the guard: a leaf
}

std::shared_ptr<const HttpServer::StrictOptions> HttpServer::strictOptions() const {
    MutexGuard guard(strictMutex_);
    return strict_;
}

bool HttpServer::admitted(httpd_req_t* req, const StrictOptions& strict) {
    std::string host = http::header(req, "Host");
    if (!host.empty() && strict.allowHost && !strict.allowHost(http::hostName(host))) {
        http::refuse(req, "421 Misdirected Request", Status::Unauthorized, Reason::NotAuthorized, "unknown host", nullptr);
        return false;
    }
    if (strict.checkOrigin) {
        std::string origin = http::header(req, "Origin");
        if (!origin.empty() && !http::originAllowed(origin, corsOrigin_, host)) {
            http::refuse(req, "403 Forbidden", Status::Unauthorized, Reason::NotAuthorized, "origin not allowed", nullptr);
            return false;
        }
    }
    return true;
}

// The transport's own token gives full access; any other goes to the API's
// token check (Api::authenticate()), with the client.
Access HttpServer::access(httpd_req_t* req, const Client& client) const {
    std::string token = http::requestToken(req);
    if (token_ && *token_ && secretEquals(token, token_)) return Access::Admin;
    return api_.tokenAccess(token, client);
}

}  // namespace tesser

#endif
