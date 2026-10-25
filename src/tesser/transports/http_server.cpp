#include "tesser/transports/http_server.h"

#if defined(ESP_PLATFORM)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>

#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/http_checks.h"
#include "tesser/stream_subscriber.h"

// mDNS (advertise()): Arduino-ESP32 ships the component; on ESP-IDF, the
// component's CMakeLists links espressif/mdns when the application has it.
#if defined(TESSER_HAVE_MDNS) || (defined(ARDUINO) && __has_include("mdns.h"))
#define TESSER_MDNS 1
#include "mdns.h"
#else
#define TESSER_MDNS 0
#endif

namespace tesser {

namespace {

constexpr size_t kChunk = 512;
constexpr size_t kFileChunk = 2048;  // file transfers, on the heap

// The stack copies what is sent (TCP's segments, then the radio's buffers)
// and holds it until the client acknowledges it: about twice it. A large
// piece waits, up to 500 ms, for the heap to hold that above the floor,
// while what went before is acknowledged (docs/DESIGN.md section 15).
// Measured on DEVKIT-V3: a 3.9 KB WebSocket frame took about 6.5 KB to
// send, and a 100 KB file download held 4-6 KB free for 2.5 s, 2 KB
// pieces sent as fast as the socket took them.
void waitForRoom(size_t bytes) {
    for (int i = 0; bytes > 1024 && i < 50 && !heapAbove(2 * bytes); i++) vTaskDelay(pdMS_TO_TICKS(10));
}
// A refused upload's body is read and dropped up to this size, so the client
// (which sends it all before reading) gets the error; beyond it, the
// connection is closed.
constexpr size_t kDrainLimit = 256 * 1024;

// Reads and drops `left` bytes of request body. False if the client went away.
bool drain(httpd_req_t* req, size_t left) {
    char buf[256];
    int timeouts = 0;
    while (left) {
        int n = httpd_req_recv(req, buf, left < sizeof(buf) ? left : sizeof(buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
        if (n <= 0) return false;
        timeouts = 0;
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

}  // namespace

// Shared between a parked HTTP handler and the deferred reply that completes
// it. Whoever lets go last frees it. With async handlers, the handler doesn't
// park: the request is answered later, from the httpd task.
struct HttpServer::DeferredState {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::atomic<int> refs{2};
    Status status = Status::Internal;
    std::string body;
    // Async (docs/DESIGN.md section 10.2): set before asyncReady.
    httpd_req_t* async = nullptr;
    HttpServer* server = nullptr;
    uint32_t deadlineMs = 0;
    std::string path;
    std::atomic<bool> asyncReady{false};
    std::atomic<bool> answered{false};

    void unref() {
        if (refs.fetch_sub(1) == 1) {
            vSemaphoreDelete(done);
            delete this;
        }
    }
};

namespace {

class DeferredHttpReply : public Reply {
public:
    using DeferredState = HttpServer::DeferredState;
    DeferredHttpReply(DeferredState* st, size_t limit) : st_(st), sink_(st->body, limit, true) {}
    Sink& begin(Status s) override {
        st_->status = s;
        st_->body.clear();
        sink_.lowMemory = false;
        return sink_;
    }
    bool outOfMemory() const override { return sink_.lowMemory; }
    void end() override {
        // Async: the server answers, from its own task. Else the parked
        // handler does, or serve() finds it done (see serve()).
        if (st_->asyncReady.load()) {
            st_->server->answerAsync(st_, false);
        } else {
            xSemaphoreGive(st_->done);
        }
    }
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

// Streams the body: what is written waits, up to a window, for drain(),
// which the core calls without the API lock (docs/DESIGN.md section 9);
// beyond the window it goes at once. A response that fits the window when it
// ends goes out whole with a Content-Length; a larger one is chunked.
class HttpReply : public Reply {
public:
    using DeferredState = HttpServer::DeferredState;
    static constexpr size_t kWindow = 2048;

    HttpReply(HttpServer& server, httpd_req_t* req) : server_(server), req_(req) {}
    ~HttpReply() override {
        if (ended_) finish();  // nobody drained it
    }

    Sink& begin(Status s) override {
        status_ = s;
        pending_.clear();
        chunked_ = false;
        ended_ = false;
        return sink_;
    }

    void end() override {
        // Under the API lock (in a request): sent by drain(), without it.
        if (server_.api().mutex().heldByMe()) {
            ended_ = true;
            return;
        }
        finish();
    }

    bool rollback() override {
        if (chunked_) return false;
        pending_.clear();
        ended_ = false;
        return true;
    }

    bool wantsDrain() const override { return !failed_ && (ended_ || pending_.size() >= kChunk); }
    void drain() override {
        if (ended_) {
            finish();
        } else if (!failed_ && pending_.size() >= kChunk) {
            sendChunk();
        }
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

    void finish() {
        ended_ = false;
        if (failed_) return;
        if (!chunked_) {
            sendHeaders();
            httpd_resp_send(req_, pending_.data(), static_cast<ssize_t>(pending_.size()));
        } else {
            sendChunk();
            httpd_resp_send_chunk(req_, nullptr, 0);
        }
        pending_.clear();
        pending_.shrink_to_fit();
    }

    bool sendChunk() {
        if (!chunked_) {
            chunked_ = true;
            sendHeaders();
        }
        if (pending_.empty()) return true;
        if (httpd_resp_send_chunk(req_, pending_.data(), static_cast<ssize_t>(pending_.size())) != ESP_OK) {
            failed_ = true;
            return false;
        }
        pending_.clear();
        return true;
    }

    bool append(const char* data, size_t len) {
        if (failed_) return false;
        // On a short heap, what is pending goes first.
        if (!growFor(pending_, pending_.size() + len) && (!sendChunk() || !growFor(pending_, len))) return false;
        pending_.append(data, len);
        return pending_.size() <= kWindow || sendChunk();
    }

    HttpServer& server_;
    httpd_req_t* req_;
    ChunkSink sink_{*this};
    std::string pending_;
    Status status_ = Status::Ok;
    bool chunked_ = false;
    bool ended_ = false;  // end() came under the API lock: drain() finishes
    bool failed_ = false;
    DeferredState* deferred_ = nullptr;
};

// One WebSocket connection: a client that can subscribe and transfer files.
class HttpServer::WsClient : public Subscriber {
public:
    WsClient(HttpServer& server, int socket) : server_(server), fd(socket), files(server.api_) {}
    // Decided at the handshake, from its token; asked again when the token
    // check's answers change (Api::tokensChanged()).
    Access access = Access::Public;
    std::string token;      // the handshake's, set before any request
    Client client;          // the handshake's
    bool checked = false;   // access comes from the token check
    uint32_t generation = 0;  // Api::tokenGeneration() when access was decided
    bool notify(const std::string& message, Delivery) override { return server_.wsSend(fd, message); }
    bool notifyShared(const std::shared_ptr<const std::string>& message, Delivery) override {
        return server_.wsSendShared(fd, message);
    }
    bool recheckLevel(Access& level) override {
        if (!checked) return false;
        level = server_.api_.tokenAccess(token, client);
        return true;
    }
    // Subscriptions ended: closed, so the client notices and connects again.
    void levelLowered() override {
        if (server_.server_) httpd_sess_trigger_close(server_.server_, fd);
    }

private:
    HttpServer& server_;

public:
    int fd;
    FileSession files;  // used in the httpd task
};

// One streamed subscription: the socket it holds and its subscriber. It is
// the socket's session context, so it goes when the socket closes
// (freeStream()).
class HttpServer::Stream {
public:
    Stream(HttpServer& owner, httpd_handle_t httpd, int socket)
        : server(&owner),
          handle(httpd),
          fd(socket),
          subscriber(owner.api_, [this](const std::string& piece) {
              HttpServer* s = this->server.load();
              return s && s->streamSend(this, piece);
          }) {}

    std::atomic<HttpServer*> server;  // null once the server ended
    httpd_handle_t handle;
    int fd;
    StreamSubscriber subscriber;
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
    std::shared_ptr<const std::string> message;  // shared with other clients' frames (events)
    Reservation held;
};
}  // namespace
#endif

// Frames are written only from the httpd task (httpd_queue_work), so
// notifications and deferred replies from other tasks never interleave
// with the server's own writes.
bool HttpServer::wsSend(int fd, const std::string& message) {
    return wsSendShared(fd, std::make_shared<const std::string>(message));
}

bool HttpServer::wsSend(int fd, std::string&& message) {
    // Taken, not copied: a reply isn't on a short heap twice while it waits
    // for the httpd task (docs/DESIGN.md section 15).
    return wsSendShared(fd, std::make_shared<const std::string>(std::move(message)));
}

bool HttpServer::wsSendShared(int fd, std::shared_ptr<const std::string> message) {
#if CONFIG_HTTPD_WS_SUPPORT
    httpd_handle_t handle = server_;
    if (!handle) return false;
    // Counted until the httpd task has sent it (docs/DESIGN.md section 15).
    Reservation held = queued_.reserve(message->size(), static_cast<uintptr_t>(fd) + 1, false, false);
    if (!held) {
        workRefused_++;
        return false;
    }
    auto* work = new WsWork{this, handle, fd, std::move(message), std::move(held)};
    esp_err_t err = httpd_queue_work(
        handle,
        [](void* arg) {
            auto* w = static_cast<WsWork*>(arg);
            w->server->workRun_++;
            bool ok = httpd_ws_get_fd_info(w->handle, w->fd) == HTTPD_WS_CLIENT_WEBSOCKET;
            if (ok) waitForRoom(w->message->size());
            if (ok) {
                httpd_ws_frame_t frame = {};
                frame.type = HTTPD_WS_TYPE_TEXT;
                frame.final = true;
                // httpd only reads the payload.
                frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(w->message->data()));
                frame.len = w->message->size();
                ok = httpd_ws_send_frame_async(w->handle, w->fd, &frame) == ESP_OK;
            }
            if (!ok) w->server->dropWsClient(w->fd);
            delete w;
        },
        work);
    if (err != ESP_OK) {
        workRefused_++;
        delete work;
        return false;
    }
    workQueued_++;
    return true;
#else
    (void)fd;
    (void)message;
    return false;
#endif
}

HttpServer::WorkStats HttpServer::workStats() const {
    WorkStats s;
    s.queued = workQueued_.load();
    s.run = workRun_.load();
    s.refused = workRefused_.load();
    return s;
}

namespace {
constexpr size_t kMaxToken = 256;

// The token a request presents ("Authorization: Bearer <token>" or
// ?token=<token>), empty if none or too long.
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
// A request header's value; empty when it is absent.
std::string header(httpd_req_t* req, const char* name) {
    size_t len = httpd_req_get_hdr_value_len(req, name);
    if (!len) return std::string();
    std::string value(len + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, name, &value[0], value.size()) != ESP_OK) return std::string();
    value.resize(len);
    return value;
}

// A refusal of the strict mode, with an error body like any other.
void refuse(httpd_req_t* req, const char* statusLine, Status status, Reason reason, const char* message,
            const char* corsOrigin) {
    StringReply body;
    writeError(body, status, reason, std::string_view(), message);
    httpd_resp_set_status(req, statusLine);
    httpd_resp_set_type(req, "application/json");
    if (corsOrigin) httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", corsOrigin);
    httpd_resp_send(req, body.body.data(), static_cast<ssize_t>(body.body.size()));
}

// The address of the client on socket `fd`, IPv4 or IPv6.
void peerAddress(int fd, Client& client) {
    struct sockaddr_in6 addr = {};
    socklen_t addrLen = sizeof(addr);
    if (fd < 0 || getpeername(fd, reinterpret_cast<struct sockaddr*>(&addr), &addrLen) != 0) return;
    if (addr.sin6_family == AF_INET) {
        memcpy(client.address, &reinterpret_cast<struct sockaddr_in*>(&addr)->sin_addr.s_addr, 4);
        client.addressLength = 4;
    } else {
        memcpy(client.address, &addr.sin6_addr, 16);
        client.addressLength = 16;
    }
}
}  // namespace

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
    std::string host = header(req, "Host");
    if (!host.empty() && strict.allowHost && !strict.allowHost(http::hostName(host))) {
        refuse(req, "421 Misdirected Request", Status::Unauthorized, Reason::NotAuthorized, "unknown host", nullptr);
        return false;
    }
    if (strict.checkOrigin) {
        std::string origin = header(req, "Origin");
        if (!origin.empty() && !http::originAllowed(origin, corsOrigin_, host)) {
            refuse(req, "403 Forbidden", Status::Unauthorized, Reason::NotAuthorized, "origin not allowed", nullptr);
            return false;
        }
    }
    return true;
}

// The transport's own token gives full access; any other goes to the API's
// token check (Api::authenticate()), with the client.
Access HttpServer::access(httpd_req_t* req, const Client& client) const {
    std::string token = requestToken(req);
    if (token_ && *token_ && secretEquals(token, token_)) return Access::Admin;
    return api_.tokenAccess(token, client);
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
    // Refused: answered, and the connection closes without switching.
    std::shared_ptr<const StrictOptions> strict = self->strictOptions();
    if (strict && !self->admitted(req, *strict)) return ESP_FAIL;
    WsClient* ws = self->wsClient(httpd_req_to_sockfd(req), true);
    ws->client.transport = TransportKind::WebSocket;
    peerAddress(httpd_req_to_sockfd(req), ws->client);
    ws->generation = self->api_.tokenGeneration();
    ws->token = requestToken(req);
    // The server's own token is admin, whatever the check says.
    ws->checked = !(self->token_ && *self->token_ && secretEquals(ws->token, self->token_));
    if (!ws->checked) ws->token.clear();
    ws->access = ws->checked ? self->api_.tokenAccess(ws->token, ws->client) : Access::Admin;
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
        EnvelopeReply reply(noId, 0, [this, fd](std::string& m) { wsSend(fd, std::move(m)); });
        writeError(reply, Status::TooLarge, std::string_view(), "message too large");
        // The payload is still unread: drop the connection.
        return ESP_FAIL;
    }
    // Room for the frame first (docs/DESIGN.md section 15): above the floor,
    // in one block with 256 bytes to spare rather than a reservation's 1 KB
    // (its bytes are in the network stack's buffers already, and it goes
    // once handled: on a fragmented gateway, 11 KB free with no block over
    // 4.6 KB refused a 3.9 KB frame). On a short heap the client hears busy,
    // and the payload is read and dropped a piece at a time, so the
    // connection stays. httpd holds no bytes of it ahead: only an HTTP
    // request's parser does.
    Reservation room = memoryBudget().reserve(frame.len + 1, 0, false, true, mallocPool(frame.len + 1));
    if (room && frame.len + 1 > 1024 && largestFreeBlock(room.pool()) < frame.len + 1 + 256) room.release();
    if (!room) {
        EnvelopeId noId;
        EnvelopeReply reply(noId, 0, [this, fd](std::string& m) { wsSend(fd, std::move(m)); });
        writeError(reply, Status::Busy, Reason::LowMemory, std::string_view(), "not enough memory for the message");
        char drop[128];
        for (size_t left = frame.len; left;) {
            int n = httpd_socket_recv(req->handle, fd, drop, left < sizeof(drop) ? left : sizeof(drop), 0);
            if (n <= 0) return ESP_FAIL;
            left -= static_cast<size_t>(n);
        }
        return ESP_OK;
    }
    std::string text(frame.len, '\0');
    room.settle();
    if (frame.len) {
        frame.payload = reinterpret_cast<uint8_t*>(&text[0]);
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) return ESP_FAIL;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    Client client;
    client.transport = TransportKind::WebSocket;
    peerAddress(fd, client);
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
    // The token check's answers changed since: asked again.
    uint32_t generation = api_.tokenGeneration();
    if (ws->checked && ws->generation != generation) {
        ws->generation = generation;
        ws->access = api_.tokenAccess(ws->token, ws->client);
    }
    client.access = ws->access;
    client.authenticated = ws->access != Access::Public;
    // The reply streams: a large one goes out as a fragmented message, sent
    // from this (the httpd) task, so no queued frame can come in between. A
    // reply that fits in one piece is queued like any frame, which keeps it
    // in order with the notifications queued before it.
    handleEnvelope(
        api_, text, client, [this, fd](std::string& m) { wsSend(fd, std::move(m)); }, ws,
        [this, req, fd](const char* data, size_t len, bool first, bool final) {
            if (first && final) return wsSend(fd, std::string(data, len));
            httpd_ws_frame_t out = {};
            out.type = first ? HTTPD_WS_TYPE_TEXT : HTTPD_WS_TYPE_CONTINUE;
            out.fragmented = true;
            out.final = final;
            out.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(data));
            out.len = len;
            return httpd_ws_send_frame(req, &out) == ESP_OK;
        },
        &ws->files);
    return ESP_OK;
#else
    (void)req;
    return ESP_FAIL;
#endif
}

esp_err_t HttpServer::advertise(const char* instance, const char* hostname) {
#if TESSER_MDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;  // started by the application
    if (hostname && (err = mdns_hostname_set(hostname)) != ESP_OK) return err;
    if (instance) mdns_instance_name_set(instance);
    std::string api = base_.empty() ? std::string("/") : base_;
    std::string id = api_.deviceIdHex();  // the device, as remote stubs show it
    mdns_txt_item_t txt[3];
    size_t n = 0;
    txt[n++] = {"api", api.c_str()};
    if (wsUri_) txt[n++] = {"ws", wsUri_};
    if (!id.empty()) txt[n++] = {"id", id.c_str()};
    if (mdns_service_exists("_tesser", "_tcp", nullptr)) mdns_service_remove("_tesser", "_tcp");
    return mdns_service_add(instance, "_tesser", "_tcp", port_, txt, n);  // the TXT is copied
#else
    (void)instance;
    (void)hostname;
    return ESP_ERR_NOT_SUPPORTED;
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
    port_ = config.server_port;
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
    if (keepAliveTimer_) {
        esp_timer_stop(keepAliveTimer_);
        esp_timer_delete(keepAliveTimer_);
        keepAliveTimer_ = nullptr;
    }
    if (asyncTimer_) {
        esp_timer_stop(asyncTimer_);
        esp_timer_delete(asyncTimer_);
        asyncTimer_ = nullptr;
    }
    // Workers serving requests below remote nodes use the server: they end
    // within the remote's timeout (bounded wait).
    for (int i = 0; i < 1100 && remoteJobs_.load() > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    // Async requests still waiting: answered as timed out.
    std::vector<DeferredState*> waiting;
    {
        MutexGuard guard(asyncMutex_);
        waiting = async_;
    }
    for (DeferredState* st : waiting) answerAsync(st, true);
    // Streams outlive the server object until their sockets close: orphaned
    // here, freed by freeStream().
    std::vector<Stream*> streams;
    {
        MutexGuard guard(streamsMutex_);  // also waits for a running keep-alive
        streams.swap(streams_);
    }
    for (Stream* s : streams) {
        s->server = nullptr;
        s->subscriber.close();
        if (!ownsServer_) httpd_sess_trigger_close(server_, s->fd);
    }
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
    std::shared_ptr<const StrictOptions> strict = self->strictOptions();
    if (strict && !self->admitted(req, *strict)) return ESP_OK;
    httpd_resp_set_status(req, "204 No Content");
    self->addCorsHeaders(req);
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t HttpServer::onRequest(httpd_req_t* req) { return static_cast<HttpServer*>(req->user_ctx)->serve(req); }

// ---- requests below remote nodes ------------------------------------------------

namespace {
constexpr int kRemoteWorkers = 2;
struct RemoteJob {
    HttpServer* server;
    httpd_req_t* req;  // async
};
}  // namespace

// A request below a remote node may wait on the peer before anything else
// (the probe for a file, a relay, a forwarded call): on a worker task of its
// own, so httpd goes on serving (docs/DESIGN.md section 10.2).
bool HttpServer::dispatchRemote(httpd_req_t* req) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    if (remoteJobs_.fetch_add(1) >= kRemoteWorkers) {
        remoteJobs_--;
        return false;
    }
    httpd_req_t* async = nullptr;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) {
        remoteJobs_--;
        return false;
    }
    auto* job = new RemoteJob{this, async};
    if (xTaskCreate(&HttpServer::remoteTask, "tesser_remote", 6144, job, 5, nullptr) != pdPASS) {
        // No room for a task: served here after all, from the async copy.
        delete job;
        serve(async, true);
        httpd_req_async_handler_complete(async);
        remoteJobs_--;
    }
    return true;
#else
    (void)req;
    return false;
#endif
}

void HttpServer::remoteTask(void* arg) {
    auto* job = static_cast<RemoteJob*>(arg);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    esp_err_t err = job->server->serve(job->req, true);
    // A failure means the connection can't carry on, as when a handler fails.
    if (err != ESP_OK) httpd_sess_trigger_close(job->req->handle, httpd_req_to_sockfd(job->req));
    httpd_req_async_handler_complete(job->req);
    job->server->remoteJobs_--;
#endif
    delete job;
    vTaskDelete(nullptr);
}

esp_err_t HttpServer::serve(httpd_req_t* req, bool inWorker) {
    std::shared_ptr<const StrictOptions> strict = strictOptions();
    if (strict && !inWorker && !admitted(req, *strict)) return ESP_OK;
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
    peerAddress(httpd_req_to_sockfd(req), request.client);
    request.client.access = access(req, request.client);
    request.client.authenticated = request.client.access != Access::Public;

    // Query options.
    char* query = nullptr;
    size_t queryLen = httpd_req_get_url_query_len(req);
    char value[16];
    std::string keys, exclude, shapeText, filename;
    bool bad = false;
    bool sub = false;
    if (queryLen) {
        query = static_cast<char*>(malloc(queryLen + 1));
        if (!query || httpd_req_get_url_query_str(req, query, queryLen + 1) != ESP_OK) {
            free(query);
            writeError(reply, Status::Internal, Reason::LowMemory, path, "out of memory");
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
        // A streamed subscription (?sub, sub=1 or sub=true) and its options.
        sub = flagParam(query, "sub", bad);
        if (queryParam(query, "interval", value, sizeof(value), len, bad)) {
            char* end = nullptr;
            unsigned long ms = strtoul(value, &end, 10);
            if (len == 0 || *end || value[0] == '-') bad = true;
            request.query.interval = static_cast<uint32_t>(ms);
        }
        boolParam(query, "changes", request.query.changes, bad);
        boolParam(query, "events", request.query.events, bad);
        boolParam(query, "snapshot", request.query.snapshot, bad);
        boolParam(query, "remotes", request.query.remotes, bad);
        // Browsers can't send a GET body, so a shape may also come as ?shape=<json>.
        shapeText.resize(queryLen + 1);
        if (queryParam(query, "shape", &shapeText[0], shapeText.size(), len, bad)) {
            shapeText.resize(len);
        } else {
            shapeText.clear();
        }
        // The file name the client gave, for a transfer (FileRequest::filename).
        filename.resize(queryLen + 1);
        if (queryParam(query, "filename", &filename[0], filename.size(), len, bad)) {
            filename.resize(len);
        } else {
            filename.clear();
        }
        free(query);
        if (bad) {
            writeError(reply, Status::BadRequest, path, "invalid query parameter");
            return ESP_OK;
        }
    }

    // Below a remote node, before anything waits on the peer: on a worker.
    // (A streamed subscription stays here, its socket kept by httpd.)
    if (!inWorker && !sub && api_.belowRemote(path) && dispatchRemote(req)) return ESP_OK;

    // File nodes (docs/DESIGN.md section 4.6): raw bytes both ways, before
    // the body is read, so maxRequestBody doesn't apply to uploads.
    if (sub && request.op != Op::Get) {
        writeError(reply, Status::BadRequest, path, "subscribe with GET");
        return ESP_OK;
    }
    if (!sub && (request.op == Op::Set || request.query.view == View::Value)) {
        std::unique_ptr<FileTransfer> owned(new FileTransfer(api_, FileTransfer::Remotes::Wait));
        FileTransfer& file = *owned;
        FileTransfer::Open o = request.op == Op::Set
                                   ? file.openUpload(path, request.client, req->content_len, reply, filename)
                                   : file.openDownload(path, request.client, reply, filename);
        if (o == FileTransfer::Open::Ok) {
            // Relayed to a remote node, it waits for each chunk's reply: off
            // the httpd task (docs/DESIGN.md section 14).
            if (!inWorker && file.relay() && relayAsync(req, owned, request.op == Op::Set)) return ESP_OK;
            return request.op == Op::Set ? receiveFile(req, file, reply) : sendFile(req, file);
        }
        if (o == FileTransfer::Open::Failed) {
            if (request.op != Op::Set || req->content_len == 0) return ESP_OK;
            return req->content_len <= kDrainLimit && drain(req, req->content_len) ? ESP_OK : ESP_FAIL;
        }
    }

    // Body. JSON only, in the strict mode: a form or text/plain needs no
    // preflight.
    if (strict && strict->jsonBodies && request.op == Op::Set && req->content_len > 0 &&
        !http::isJsonType(header(req, "Content-Type"))) {
        refuse(req, "415 Unsupported Media Type", Status::BadRequest, Reason::None, "the body must be application/json",
               corsOrigin_);
        return ESP_OK;
    }
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
                                                   DeserializationOption::NestingLimit(requestNesting(cfg.maxDepth)));
        if (err) {
            writeError(reply, Status::BadRequest, path, "malformed JSON body");
            return ESP_OK;
        }
        request.body = doc.as<JsonVariantConst>();
    }
    if (sub) return serveStream(req, request, reply);

    api_.handle(request, reply);

    if (DeferredState* st = reply.deferred()) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        // The httpd task doesn't wait: the request turns async, answered
        // when the reply comes (or deferTimeoutMs passes), and the server
        // goes on serving everyone else (docs/DESIGN.md section 10.2).
        httpd_req_t* async = nullptr;
        if (!inWorker && httpd_req_async_handler_begin(req, &async) == ESP_OK) {
            st->async = async;
            st->server = this;
            st->deadlineMs = millis32() + cfg.deferTimeoutMs;
            st->path = path;
            watchAsync(st);
            st->asyncReady = true;
            // It may have ended before asyncReady: then it gave `done`.
            if (xSemaphoreTake(st->done, 0) == pdTRUE) answerAsync(st, false);
            return ESP_OK;
        }
#endif
        if (xSemaphoreTake(st->done, pdMS_TO_TICKS(cfg.deferTimeoutMs)) == pdTRUE) {
            reply.sendWhole(st->status, st->body);
        } else {
            writeError(reply, Status::Timeout, Reason::NoAnswer, path, "deferred call timed out");
        }
        st->unref();
    }
    return ESP_OK;
}

// ---- streamed subscriptions ------------------------------------------------

size_t HttpServer::streams() const {
    MutexGuard guard(streamsMutex_);
    return streams_.size();
}

namespace {
struct StreamWork {
    HttpServer* server;
    httpd_handle_t handle;
    void* stream;
    int fd;
    std::string chunk;
    Reservation held;
};
}  // namespace

// Like WebSocket frames, chunks are written only from the httpd task
// (httpd_queue_work), so writes from other tasks never interleave.
bool HttpServer::streamSend(Stream* stream, const std::string& piece) {
    char size[12];
    int n = snprintf(size, sizeof(size), "%x\r\n", static_cast<unsigned>(piece.size()));
    // The chunk, built in one allocation and counted until sent.
    size_t len = static_cast<size_t>(n) + piece.size() + 2;
    Reservation held = queued_.reserve(len, static_cast<uintptr_t>(stream->fd) + 1, true, true, mallocPool(len));
    if (!held) {
        workRefused_++;
        return false;
    }
    std::string chunk;
    chunk.reserve(len);
    held.settle();
    chunk.append(size, static_cast<size_t>(n));
    chunk += piece;
    chunk += "\r\n";
    auto* work = new StreamWork{this, stream->handle, stream, stream->fd, std::move(chunk), std::move(held)};
    esp_err_t err = httpd_queue_work(
        stream->handle,
        [](void* arg) {
            auto* w = static_cast<StreamWork*>(arg);
            w->server->workRun_++;
            bool live = false;
            {
                // Still open, and the socket still its own: a closed socket's
                // descriptor may already serve another connection.
                MutexGuard guard(w->server->streamsMutex_);
                for (Stream* s : w->server->streams_) {
                    if (s == w->stream) live = httpd_sess_get_ctx(w->handle, w->fd) == s;
                }
            }
            if (live) {
                const char* data = w->chunk.data();
                size_t left = w->chunk.size();
                while (left) {
                    int n = httpd_socket_send(w->handle, w->fd, data, left, 0);
                    if (n <= 0) break;
                    data += n;
                    left -= static_cast<size_t>(n);
                }
                if (left) {
                    httpd_sess_trigger_close(w->handle, w->fd);  // freeStream() follows
                } else {
                    // Keeps the stream from being the first socket purged.
                    httpd_sess_update_lru_counter(w->handle, w->fd);
                }
            }
            delete w;
        },
        work);
    if (err != ESP_OK) {
        workRefused_++;
        delete work;
        return false;
    }
    workQueued_++;
    return true;
}

void HttpServer::freeStream(void* ctx) {
    auto* stream = static_cast<Stream*>(ctx);
    if (HttpServer* self = stream->server.load()) {
        MutexGuard guard(self->streamsMutex_);
        for (size_t i = 0; i < self->streams_.size(); i++) {
            if (self->streams_[i] == stream) {
                self->streams_.erase(self->streams_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    delete stream;  // drops its subscription
}

// ---- relayed transfers ---------------------------------------------------------

namespace {
struct RelayJob {
    HttpServer* server;
    httpd_req_t* req;  // async
    std::unique_ptr<FileTransfer> file;
    bool upload;
};
}  // namespace

bool HttpServer::relayAsync(httpd_req_t* req, std::unique_ptr<FileTransfer>& file, bool upload) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    httpd_req_t* async = nullptr;
    if (httpd_req_async_handler_begin(req, &async) != ESP_OK) return false;
    auto* job = new RelayJob{this, async, std::move(file), upload};
    if (xTaskCreate(&HttpServer::relayTask, "tesser_relay", 4096, job, 5, nullptr) == pdPASS) return true;
    // No task: done here after all, as without async handlers.
    file = std::move(job->file);
    httpd_req_async_handler_complete(async);
    delete job;
    return false;
#else
    (void)req;
    (void)file;
    (void)upload;
    return false;
#endif
}

void HttpServer::relayTask(void* arg) {
    auto* job = static_cast<RelayJob*>(arg);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    esp_err_t err;
    {
        HttpReply reply(*job->server, job->req);
        err = job->upload ? job->server->receiveFile(job->req, *job->file, reply)
                          : job->server->sendFile(job->req, *job->file);
    }
    job->file.reset();
    // A failure means the connection can't carry on: closed, as httpd closes
    // it when a handler fails.
    if (err != ESP_OK) httpd_sess_trigger_close(job->req->handle, httpd_req_to_sockfd(job->req));
    httpd_req_async_handler_complete(job->req);
#endif
    delete job;
    vTaskDelete(nullptr);
}

// ---- async deferred replies ---------------------------------------------------

void HttpServer::watchAsync(DeferredState* st) {
    MutexGuard guard(asyncMutex_);
    async_.push_back(st);  // holds serve()'s reference until answered
    if (!asyncTimer_) {
        esp_timer_create_args_t args = {};
        args.callback = &HttpServer::onAsyncTimer;
        args.arg = this;
        args.name = "tesser_async";
        if (esp_timer_create(&args, &asyncTimer_) != ESP_OK) asyncTimer_ = nullptr;
    }
    if (asyncTimer_ && !esp_timer_is_active(asyncTimer_)) esp_timer_start_periodic(asyncTimer_, 500 * 1000);
}

// Once, by the reply or the timeout: the answer is written by the httpd task,
// like every other write, and the request completed.
void HttpServer::answerAsync(DeferredState* st, bool timedOut) {
    if (st->answered.exchange(true)) return;
    {
        MutexGuard guard(asyncMutex_);
        for (size_t i = 0; i < async_.size(); i++) {
            if (async_[i] != st) continue;
            async_.erase(async_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (timedOut) {
        StringReply r;
        writeError(r, Status::Timeout, Reason::NoAnswer, st->path, "deferred call timed out");
        st->status = r.status;
        st->body = std::move(r.body);
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    auto work = [](void* arg) {
        auto* s = static_cast<DeferredState*>(arg);
        httpd_resp_set_status(s->async, statusLine(s->status));
        httpd_resp_set_type(s->async, "application/json");
        s->server->addCorsHeaders(s->async);
        httpd_resp_send(s->async, s->body.data(), static_cast<ssize_t>(s->body.size()));
        httpd_req_async_handler_complete(s->async);
        s->unref();
    };
    if (!server_ || httpd_queue_work(server_, work, st) != ESP_OK) {
        // The server is going: complete it here.
        httpd_req_async_handler_complete(st->async);
        st->unref();
    }
#else
    st->unref();  // never watched: requests turn async only with ESP-IDF 5.3 and later
#endif
}

void HttpServer::onAsyncTimer(void* arg) {
    auto* self = static_cast<HttpServer*>(arg);
    std::vector<DeferredState*> expired;
    uint32_t now = millis32();
    {
        MutexGuard guard(self->asyncMutex_);
        for (DeferredState* st : self->async_) {
            if (static_cast<int32_t>(now - st->deadlineMs) >= 0) expired.push_back(st);
        }
        if (self->async_.size() == expired.size()) esp_timer_stop(self->asyncTimer_);
    }
    for (DeferredState* st : expired) self->answerAsync(st, true);
}

void HttpServer::onKeepAlive(void* arg) {
    auto* self = static_cast<HttpServer*>(arg);
    MutexGuard guard(self->streamsMutex_);
    uint32_t now = millis32();
    for (Stream* s : self->streams_) {
        // Its subscription ended (Api::tokensChanged()): closed.
        if (s->subscriber.takeRevoked()) {
            if (s->server.load()) httpd_sess_trigger_close(s->handle, s->fd);
            continue;
        }
        s->subscriber.keepAlive(now, self->keepAliveMs_);
    }
}

esp_err_t HttpServer::serveStream(httpd_req_t* req, const Request& request, Reply& reply) {
    {
        MutexGuard guard(streamsMutex_);
        if (streams_.size() >= maxStreams_) {
            writeError(reply, Status::Busy, Reason::SubscriptionLimit, request.path, "too many streams");
            return ESP_OK;
        }
    }
    if (!keepAliveTimer_ && keepAliveMs_) {
        esp_timer_create_args_t args = {};
        args.callback = &HttpServer::onKeepAlive;
        args.arg = this;
        args.name = "tesser_stream";
        if (esp_timer_create(&args, &keepAliveTimer_) == ESP_OK) {
            // Checked three times per period: a stream is silent for at most
            // 5/3 of it.
            esp_timer_start_periodic(keepAliveTimer_, static_cast<uint64_t>(keepAliveMs_) * 1000 / 3);
        } else {
            keepAliveTimer_ = nullptr;
        }
    }
    auto* stream = new Stream(*this, req->handle, httpd_req_to_sockfd(req));
    std::string token = requestToken(req);
    stream->subscriber.presented(token, token_ && *token_ && secretEquals(token, token_));
    {
        MutexGuard guard(streamsMutex_);
        streams_.push_back(stream);
    }
    // Nothing is written before the subscription is accepted, so a refusal
    // is an ordinary response. The snapshot and the first notifications
    // are queued: they go out after the head, once this handler returns.
    bool ok = stream->subscriber.open(request, reply) == Status::Ok;
    if (ok) {
        std::string head =
            "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nTransfer-Encoding: chunked\r\n"
            "Cache-Control: no-store\r\n";
        if (corsOrigin_) {
            head += "Access-Control-Allow-Origin: ";
            head += corsOrigin_;
            head += "\r\n";
        }
        head += "\r\n";
        ok = sendAll(req, head.data(), head.size());
    }
    if (!ok) {
        {
            MutexGuard guard(streamsMutex_);
            for (size_t i = 0; i < streams_.size(); i++) {
                if (streams_[i] == stream) streams_.erase(streams_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        delete stream;
        return ESP_OK;
    }
    // The socket's session context: freed, with the stream, when it closes.
    req->sess_ctx = stream;
    req->free_ctx = &HttpServer::freeStream;
    return ESP_OK;
}

esp_err_t HttpServer::receiveFile(httpd_req_t* req, FileTransfer& file, Reply& reply) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kFileChunk]);
    size_t left = req->content_len;
    int timeouts = 0;
    while (left) {
        int n = httpd_req_recv(req, reinterpret_cast<char*>(buf.get()), left < kFileChunk ? left : kFileChunk);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
        if (n <= 0) return ESP_FAIL;  // the client went away: the transfer is aborted
        timeouts = 0;
        left -= static_cast<size_t>(n);
        if (!file.write(buf.get(), static_cast<size_t>(n), reply)) {
            return left <= kDrainLimit && drain(req, left) ? ESP_OK : ESP_FAIL;
        }
    }
    file.finish(reply);
    return ESP_OK;
}

namespace {
// attachment; filename="<ASCII>", plus filename*=UTF-8''<percent-encoded>
// (RFC 6266) for other names. Names may come from a file system: quotes,
// backslashes and control bytes would break the header.
std::string contentDisposition(const std::string& name) {
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

}  // namespace

esp_err_t HttpServer::sendFile(httpd_req_t* req, FileTransfer& file) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kFileChunk]);
    std::string disposition = contentDisposition(file.filename());
    int64_t size = file.size();
    if (size < 0) {
        // Unknown size: chunked.
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, file.contentType());
        httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
        addCorsHeaders(req);
        int n;
        while ((n = file.read(buf.get(), kFileChunk)) > 0) {
            waitForRoom(kFileChunk);  // whatever the piece: the stack's buffer fills over many
            if (httpd_resp_send_chunk(req, reinterpret_cast<char*>(buf.get()), n) != ESP_OK) return ESP_FAIL;
        }
        if (n < 0) return ESP_FAIL;  // cut short: the client sees no final chunk
        return httpd_resp_send_chunk(req, nullptr, 0);
    }
    // Known size: a Content-Length, so clients can show progress.
    // esp_http_server can't stream with one, so the head is written here.
    char length[24];
    snprintf(length, sizeof(length), "%llu", static_cast<unsigned long long>(size));
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: ";
    head += file.contentType();
    head += "\r\nContent-Length: ";
    head += length;
    head += "\r\nContent-Disposition: ";
    head += disposition;
    head += "\r\n";
    if (corsOrigin_) {
        head += "Access-Control-Allow-Origin: ";
        head += corsOrigin_;
        head += "\r\nAccess-Control-Expose-Headers: Content-Disposition, Content-Length\r\n";
    }
    head += "\r\n";
    if (!sendAll(req, head.data(), head.size())) return ESP_FAIL;
    uint64_t left = static_cast<uint64_t>(size);
    while (left) {
        int n = file.read(buf.get(), kFileChunk);
        if (n <= 0 || static_cast<uint64_t>(n) > left) return ESP_FAIL;  // the client sees it short
        waitForRoom(kFileChunk);  // whatever the piece: the stack's buffer fills over many
        if (!sendAll(req, reinterpret_cast<char*>(buf.get()), static_cast<size_t>(n))) return ESP_FAIL;
        left -= static_cast<uint64_t>(n);
    }
    return ESP_OK;
}

}  // namespace tesser

#endif
