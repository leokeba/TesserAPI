#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

#include <stdio.h>

#include "esp_timer.h"

// Streamed subscriptions on HttpServer (docs/DESIGN.md section 11).

namespace tesser {

using namespace http;

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
bool HttpServer::streamSend(Stream* stream, const StreamSubscriber::Piece& piece) {
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
    piece.appendTo(chunk);
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
        appendCors(head);
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

}  // namespace tesser

#endif
