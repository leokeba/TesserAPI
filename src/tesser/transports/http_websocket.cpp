#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

#include "lwip/sockets.h"

// WebSocket connections on HttpServer (docs/DESIGN.md section 10.5).

namespace tesser {

using namespace http;

// Sends a reply to a WebSocket client: one type, for one std::function manager.
struct HttpServer::WsReply {
    HttpServer* server;
    int fd;
    void operator()(std::string& message) const { server->wsSend(fd, std::move(message)); }
};

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
    // A new connection: with attach(), whose server's close_fn isn't ours, a
    // closed socket's client may still be there under the reused descriptor,
    // with its subscriptions and level.
    int fd = httpd_req_to_sockfd(req);
    self->dropWsClient(fd);
    WsClient* ws = self->wsClient(fd, true);
    ws->client.transport = TransportKind::WebSocket;
    peerAddress(fd, ws->client);
    ws->generation = self->api_.tokenGeneration();
    ws->token.token = requestToken(req);
    // The server's own token is admin, whatever the check says.
    ws->token.fixed = self->token_ && *self->token_ && secretEquals(ws->token.token, self->token_);
    if (ws->token.fixed) ws->token.token.clear();
    ws->access = Access::Admin;
    ws->token.recheck(self->api_, ws->client, ws->access);
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
        EnvelopeReply reply(noId, 0, WsReply{this, fd});
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
        EnvelopeReply reply(noId, 0, WsReply{this, fd});
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
    if (ws->generation != generation) {
        ws->generation = generation;
        ws->token.recheck(api_, ws->client, ws->access);
    }
    client.access = ws->access;
    client.authenticated = ws->access != Access::Public;
    // The reply streams: a large one goes out as a fragmented message, sent
    // from this (the httpd) task, so no queued frame can come in between. A
    // reply that fits in one piece is queued like any frame, which keeps it
    // in order with the notifications queued before it.
    handleEnvelope(
        api_, text, client, WsReply{this, fd}, ws,
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

}  // namespace tesser

#endif
