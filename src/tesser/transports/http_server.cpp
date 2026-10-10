#include "tesser/transports/http_server.h"

#if defined(ESP_PLATFORM)

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "tesser/envelope.h"

namespace tesser {

namespace {

constexpr size_t kChunk = 512;

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

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Percent-decodes in place ('+' means space in query values only).
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

// Shared between a parked HTTP handler and the deferred reply that completes
// it. Whoever lets go last frees it.
struct DeferredState {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::atomic<int> refs{2};
    Status status = Status::Internal;
    std::string body;

    void unref() {
        if (refs.fetch_sub(1) == 1) {
            vSemaphoreDelete(done);
            delete this;
        }
    }
};

class DeferredHttpReply : public Reply {
public:
    DeferredHttpReply(DeferredState* st, size_t limit) : st_(st), sink_(st->body, limit) {}
    Sink& begin(Status s) override {
        st_->status = s;
        st_->body.clear();
        return sink_;
    }
    void end() override { xSemaphoreGive(st_->done); }
    bool rollback() override {
        st_->body.clear();
        return true;
    }
    void release() override {
        st_->unref();
        delete this;
    }

private:
    DeferredState* st_;
    StringSink sink_;
};

}  // namespace

// Streams the body in kChunk pieces. Responses that fit in one chunk go out
// with a Content-Length; larger ones switch to chunked encoding.
class HttpReply : public Reply {
public:
    // The chunk buffer lives on the heap: the httpd task's stack is precious.
    HttpReply(HttpServer& server, httpd_req_t* req) : server_(server), req_(req), buf_(new char[kChunk]) {}

    Sink& begin(Status s) override {
        status_ = s;
        used_ = 0;
        chunked_ = false;
        return sink_;
    }

    void end() override {
        if (failed_) return;
        if (!chunked_) {
            sendHeaders();
            httpd_resp_send(req_, buf_.get(), static_cast<ssize_t>(used_));
        } else {
            flush();
            httpd_resp_send_chunk(req_, nullptr, 0);
        }
    }

    bool rollback() override {
        if (chunked_) return false;
        used_ = 0;
        return true;
    }

    Reply* detach() override {
        if (deferred_) return nullptr;
        deferred_ = new DeferredState();
        if (!deferred_->done) {
            delete deferred_;
            deferred_ = nullptr;
            return nullptr;
        }
        return new DeferredHttpReply(deferred_, server_.api().config().maxResponse);
    }

    DeferredState* deferred() const { return deferred_; }

    // Sends a complete response produced elsewhere (deferred replies).
    void sendWhole(Status s, const std::string& body) {
        status_ = s;
        sendHeaders();
        httpd_resp_send(req_, body.data(), static_cast<ssize_t>(body.size()));
    }

private:
    class ChunkSink : public Sink {
    public:
        explicit ChunkSink(HttpReply& r) : r_(r) {}
        bool write(const char* data, size_t len) override { return r_.append(data, len); }

    private:
        HttpReply& r_;
    };

    void sendHeaders() {
        httpd_resp_set_status(req_, statusLine(status_));
        httpd_resp_set_type(req_, "application/json");
        server_.addCorsHeaders(req_);
    }

    bool flush() {
        if (used_ == 0) return true;
        if (httpd_resp_send_chunk(req_, buf_.get(), static_cast<ssize_t>(used_)) != ESP_OK) {
            failed_ = true;
            return false;
        }
        used_ = 0;
        return true;
    }

    bool append(const char* data, size_t len) {
        if (failed_) return false;
        while (len) {
            if (used_ == kChunk) {
                if (!chunked_) {
                    chunked_ = true;
                    sendHeaders();
                }
                if (!flush()) return false;
            }
            size_t n = len < kChunk - used_ ? len : kChunk - used_;
            memcpy(buf_.get() + used_, data, n);
            used_ += n;
            data += n;
            len -= n;
        }
        return true;
    }

    HttpServer& server_;
    httpd_req_t* req_;
    ChunkSink sink_{*this};
    std::unique_ptr<char[]> buf_;
    size_t used_ = 0;
    Status status_ = Status::Ok;
    bool chunked_ = false;
    bool failed_ = false;
    DeferredState* deferred_ = nullptr;
};

// One WebSocket connection: a client that can subscribe.
class HttpServer::WsClient : public Subscriber {
public:
    WsClient(HttpServer& server, int socket) : server_(server), fd(socket) {}
    bool authenticated = false;
    bool notify(const std::string& message, Delivery) override { return server_.wsSend(fd, message); }

private:
    HttpServer& server_;

public:
    int fd;
};

HttpServer::HttpServer(Api& api) : api_(api) {}

HttpServer::~HttpServer() { end(); }

size_t HttpServer::webSocketClients() const {
    MutexGuard guard(wsMutex_);
    return wsClients_.size();
}

HttpServer::WsClient* HttpServer::wsClient(int fd, bool create) {
    MutexGuard guard(wsMutex_);
    for (WsClient* c : wsClients_) {
        if (c->fd == fd) return c;
    }
    if (!create) return nullptr;
    auto* c = new WsClient(*this, fd);
    wsClients_.push_back(c);
    return c;
}

void HttpServer::dropWsClient(int fd) {
    WsClient* gone = nullptr;
    {
        MutexGuard guard(wsMutex_);
        for (size_t i = 0; i < wsClients_.size(); i++) {
            if (wsClients_[i]->fd == fd) {
                gone = wsClients_[i];
                wsClients_.erase(wsClients_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    if (!gone) return;
    api_.dropSubscriber(gone);  // waits for any notify() in progress
    delete gone;
}

#if CONFIG_HTTPD_WS_SUPPORT
namespace {
struct WsWork {
    HttpServer* server;
    httpd_handle_t handle;
    int fd;
    std::string message;
};
}  // namespace
#endif

// Frames are written only from the httpd task (httpd_queue_work), so
// notifications and deferred replies from other tasks never interleave
// with the server's own writes.
bool HttpServer::wsSend(int fd, const std::string& message) {
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_handle_t handle = server_;
    if (!handle) return false;
    auto* work = new WsWork{this, handle, fd, message};
    esp_err_t err = httpd_queue_work(
        handle,
        [](void* arg) {
            auto* w = static_cast<WsWork*>(arg);
            bool ok = httpd_ws_get_fd_info(w->handle, w->fd) == HTTPD_WS_CLIENT_WEBSOCKET;
            if (ok) {
                httpd_ws_frame_t frame = {};
                frame.type = HTTPD_WS_TYPE_TEXT;
                frame.final = true;
                frame.payload = reinterpret_cast<uint8_t*>(&w->message[0]);
                frame.len = w->message.size();
                ok = httpd_ws_send_frame_async(w->handle, w->fd, &frame) == ESP_OK;
            }
            if (!ok) w->server->dropWsClient(w->fd);
            delete w;
        },
        work);
    if (err != ESP_OK) {
        delete work;
        return false;
    }
    return true;
#else
    (void)fd;
    (void)message;
    return false;
#endif
}

bool HttpServer::authenticated(httpd_req_t* req) const {
    if (!token_ || !*token_) return false;
    size_t tokenLen = strlen(token_);
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 7 + tokenLen) {
        std::string value(len + 1, '\0');
        if (httpd_req_get_hdr_value_str(req, "Authorization", &value[0], value.size()) == ESP_OK &&
            memcmp(value.data(), "Bearer ", 7) == 0 && memcmp(value.data() + 7, token_, tokenLen) == 0) {
            return true;
        }
    }
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen) {
        std::string query(qlen + 1, '\0');
        std::string value(tokenLen + 2, '\0');
        if (httpd_req_get_url_query_str(req, &query[0], query.size()) == ESP_OK &&
            httpd_query_key_value(query.c_str(), "token", &value[0], value.size()) == ESP_OK &&
            strcmp(value.c_str(), token_) == 0) {
            return true;
        }
    }
    return false;
}

void HttpServer::onClose(httpd_handle_t server, int fd) {
    auto* self = static_cast<HttpServer*>(httpd_get_global_user_ctx(server));
    if (self) self->dropWsClient(fd);
    close(fd);
}

esp_err_t HttpServer::onWebSocket(httpd_req_t* req) {
    return static_cast<HttpServer*>(req->user_ctx)->serveWebSocket(req);
}

// The handshake's headers and query are only readable here. ESP-IDF calls
// this before switching protocols (pre-handshake callback); before 6.1 it
// also called the WebSocket handler with the handshake's GET.
esp_err_t HttpServer::onWebSocketHandshake(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);
    self->wsClient(httpd_req_to_sockfd(req), true)->authenticated = self->authenticated(req);
    return ESP_OK;
}

esp_err_t HttpServer::serveWebSocket(httpd_req_t* req) {
#if CONFIG_HTTPD_WS_SUPPORT
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) return onWebSocketHandshake(req);  // ESP-IDF before 6.1
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    if (frame.len > api_.config().maxRequestBody) {
        EnvelopeId noId;
        EnvelopeReply reply(noId, 0, [this, fd](const std::string& m) { wsSend(fd, m); });
        writeError(reply, Status::TooLarge, std::string_view(), "message too large");
        // The payload is still unread: drop the connection.
        return ESP_FAIL;
    }
    std::string text(frame.len, '\0');
    if (frame.len) {
        frame.payload = reinterpret_cast<uint8_t*>(&text[0]);
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    Client client;
    client.transport = TransportKind::WebSocket;
    struct sockaddr_in6 addr = {};
    socklen_t addrLen = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) == 0 && addr.sin6_family == AF_INET) {
        memcpy(client.address, &reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr, 4);
        client.addressLength = 4;
    }
    WsClient* ws = wsClient(fd, false);
    if (!ws) {
        // The handshake went unseen, so its token can't count.
        if (token_ && *token_ && !wsAuthWarned_) {
            wsAuthWarned_ = true;
            logWarning("WebSocket handshakes aren't visible: enable CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT "
                       "for token authentication");
        }
        ws = wsClient(fd, true);
    }
    client.authenticated = ws->authenticated;
    handleEnvelope(api_, text, client, [this, fd](const std::string& m) { wsSend(fd, m); }, ws);
    return ESP_OK;
#else
    (void)req;
    return ESP_FAIL;
#endif
}

esp_err_t HttpServer::begin(uint16_t port, const char* basePath) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = static_cast<uint16_t>(config.ctrl_port + port % 1000);
    // Handlers render recursively and lwIP sends from this stack: 8 KB keeps
    // a comfortable margin (measured in test/hardware).
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 10;
    return begin(config, basePath);
}

esp_err_t HttpServer::begin(httpd_config_t config, const char* basePath) {
    if (server_) return ESP_ERR_INVALID_STATE;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (wsUri_) {
        // Notices closed WebSocket connections at once.
        config.global_user_ctx = this;
        config.global_user_ctx_free_fn = [](void*) {};
        config.close_fn = &HttpServer::onClose;
    }
    httpd_handle_t handle = nullptr;
    esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK) return err;
    err = attach(handle, basePath);
    if (err != ESP_OK) {
        httpd_stop(handle);
        return err;
    }
    ownsServer_ = true;
    return ESP_OK;
}

esp_err_t HttpServer::attach(httpd_handle_t server, const char* basePath) {
    if (server_) return ESP_ERR_INVALID_STATE;
    base_ = basePath ? basePath : "";
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    pattern_ = base_ + "*";

    // First, so the API's wildcard (e.g. "/*" for an empty base) can't shadow it.
#if CONFIG_HTTPD_WS_SUPPORT
    if (wsUri_) {
        httpd_uri_t uri = {};
        uri.uri = wsUri_;
        uri.method = HTTP_GET;
        uri.handler = &HttpServer::onWebSocket;
        uri.user_ctx = this;
        uri.is_websocket = true;
#if CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT
        uri.ws_pre_handshake_cb = &HttpServer::onWebSocketHandshake;
#endif
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) return err;
    }
#endif

    const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_PATCH};
    for (httpd_method_t m : methods) {
        httpd_uri_t uri = {};
        uri.uri = pattern_.c_str();
        uri.method = m;
        uri.handler = &HttpServer::onRequest;
        uri.user_ctx = this;
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            for (httpd_method_t r : methods) {
                if (r == m) break;
                httpd_unregister_uri_handler(server, pattern_.c_str(), r);
            }
            return err;
        }
    }
    if (corsOrigin_) {
        httpd_uri_t uri = {};
        uri.uri = pattern_.c_str();
        uri.method = HTTP_OPTIONS;
        uri.handler = &HttpServer::onOptions;
        uri.user_ctx = this;
        httpd_register_uri_handler(server, &uri);
    }
    server_ = server;
    return ESP_OK;
}

void HttpServer::end() {
    if (!server_) return;
    for (;;) {
        int fd = -1;
        {
            MutexGuard guard(wsMutex_);
            if (wsClients_.empty()) break;
            fd = wsClients_.front()->fd;
        }
        dropWsClient(fd);
    }
    if (ownsServer_) {
        httpd_stop(server_);
    } else {
        const httpd_method_t methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_PATCH, HTTP_OPTIONS};
        for (httpd_method_t m : methods) httpd_unregister_uri_handler(server_, pattern_.c_str(), m);
        if (wsUri_) httpd_unregister_uri_handler(server_, wsUri_, HTTP_GET);
    }
    server_ = nullptr;
    ownsServer_ = false;
}

void HttpServer::addCorsHeaders(httpd_req_t* req) {
    if (!corsOrigin_) return;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", corsOrigin_);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
}

esp_err_t HttpServer::onOptions(httpd_req_t* req) {
    auto* self = static_cast<HttpServer*>(req->user_ctx);
    httpd_resp_set_status(req, "204 No Content");
    self->addCorsHeaders(req);
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpServer::onRequest(httpd_req_t* req) { return static_cast<HttpServer*>(req->user_ctx)->serve(req); }

esp_err_t HttpServer::serve(httpd_req_t* req) {
    HttpReply reply(*this, req);
    const Config& cfg = api_.config();

    // Path: strip the base, then the query string, then percent-decode.
    const char* uri = req->uri;
    size_t uriLen = strlen(uri);
    size_t q = 0;
    while (q < uriLen && uri[q] != '?') q++;
    if (q < base_.size() || memcmp(uri, base_.data(), base_.size()) != 0 ||
        (q > base_.size() && uri[base_.size()] != '/')) {
        writeError(reply, Status::NotFound, std::string_view(uri, q), "outside the API");
        return ESP_OK;
    }
    std::string path(uri + base_.size(), q - base_.size());
    path.resize(urlDecode(&path[0], path.size(), false));
    if (path.empty()) path = "/";

    Request request;
    request.op = req->method == HTTP_GET ? Op::Get : Op::Set;
    request.path = path;
    request.client.transport = TransportKind::Http;
    request.client.authenticated = authenticated(req);
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 addr = {};
    socklen_t addrLen = sizeof(addr);
    if (fd >= 0 && getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) == 0) {
        if (addr.sin6_family == AF_INET) {
            auto* a4 = reinterpret_cast<struct sockaddr_in*>(&addr);
            memcpy(request.client.address, &a4->sin_addr.s_addr, 4);
            request.client.addressLength = 4;
        } else {
            memcpy(request.client.address, &addr.sin6_addr, 16);
            request.client.addressLength = 16;
        }
    }

    // Query options.
    char* query = nullptr;
    size_t queryLen = httpd_req_get_url_query_len(req);
    char value[16];
    std::string keys, exclude, shapeText;
    bool bad = false;
    if (queryLen) {
        query = static_cast<char*>(malloc(queryLen + 1));
        if (!query || httpd_req_get_url_query_str(req, query, queryLen + 1) != ESP_OK) {
            free(query);
            writeError(reply, Status::Internal, path, "out of memory");
            return ESP_OK;
        }
        size_t len = 0;
        if (queryParam(query, "depth", value, sizeof(value), len, bad)) {
            char* end = nullptr;
            long d = strtol(value, &end, 10);
            if (len == 0 || *end || d < 0 || d > 255) bad = true;
            request.query.depth = static_cast<int>(d);
        }
        keys.resize(queryLen + 1);
        if (queryParam(query, "keys", &keys[0], keys.size(), len, bad)) {
            keys.resize(len);
            request.query.keys = keys;
        }
        exclude.resize(queryLen + 1);
        if (queryParam(query, "exclude", &exclude[0], exclude.size(), len, bad)) {
            exclude.resize(len);
            request.query.exclude = exclude;
        }
        if (queryParam(query, "view", value, sizeof(value), len, bad) &&
            !parseView(std::string_view(value, len), request.query.view)) {
            bad = true;
        }
        // Browsers can't send a GET body, so a shape may also come as ?shape=<json>.
        shapeText.resize(queryLen + 1);
        if (queryParam(query, "shape", &shapeText[0], shapeText.size(), len, bad)) {
            shapeText.resize(len);
        } else {
            shapeText.clear();
        }
        free(query);
        if (bad) {
            writeError(reply, Status::BadRequest, path, "invalid query parameter");
            return ESP_OK;
        }
    }

    // Body.
    if (req->content_len > cfg.maxRequestBody) {
        writeError(reply, Status::TooLarge, path, "request body too large");
        return ESP_OK;
    }
    std::string body;
    if (req->content_len > 0) {
        body.resize(req->content_len);
        size_t got = 0;
        while (got < body.size()) {
            int n = httpd_req_recv(req, &body[got], body.size() - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
            if (n <= 0) return ESP_FAIL;
            got += static_cast<size_t>(n);
        }
    }
    if (!body.empty() && !shapeText.empty()) {
        writeError(reply, Status::BadRequest, path, "shape given twice");
        return ESP_OK;
    }
    const std::string& json = body.empty() ? shapeText : body;
    JsonDocument doc;
    if (!json.empty()) {
        DeserializationError err = deserializeJson(doc, json.data(), json.size(),
                                                   DeserializationOption::NestingLimit(cfg.maxDepth + 1));
        if (err) {
            writeError(reply, Status::BadRequest, path, "malformed JSON body");
            return ESP_OK;
        }
        request.body = doc.as<JsonVariantConst>();
    }

    api_.handle(request, reply);

    if (DeferredState* st = reply.deferred()) {
        if (xSemaphoreTake(st->done, pdMS_TO_TICKS(cfg.deferTimeoutMs)) == pdTRUE) {
            reply.sendWhole(st->status, st->body);
        } else {
            writeError(reply, Status::Timeout, path, "deferred call timed out");
        }
        st->unref();
    }
    return ESP_OK;
}

}  // namespace tesser

#endif
