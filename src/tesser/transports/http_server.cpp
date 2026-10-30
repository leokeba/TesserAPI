#include "tesser/transports/http_server.h"

#if defined(ESP_PLATFORM)

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>

#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "tesser/transports/http_internal.h"

// The server's life and the dispatch of requests. Its other parts have files
// of their own: http_request.cpp, http_websocket.cpp, http_streams.cpp,
// http_async.cpp, http_files.cpp, http_mdns.cpp.

namespace tesser {

using namespace http;

HttpReply::~HttpReply() {
    if (ended_) finish();  // nobody drained it
}

Sink& HttpReply::begin(Status s) {
    status_ = s;
    pending_.clear();
    chunked_ = false;
    ended_ = false;
    return sink_;
}

void HttpReply::end() {
    // Under the API lock (in a request): sent by drain(), without it.
    if (server_.api().mutex().heldByMe()) {
        ended_ = true;
        return;
    }
    finish();
}

bool HttpReply::rollback() {
    if (chunked_) return false;
    pending_.clear();
    ended_ = false;
    return true;
}

void HttpReply::drain() {
    if (ended_) {
        finish();
    } else if (!failed_ && pending_.size() >= kChunk) {
        sendChunk();
    }
}

void HttpReply::sendWhole(Status s, const std::string& body) {
    status_ = s;
    sendHeaders();
    httpd_resp_send(req_, body.data(), static_cast<ssize_t>(body.size()));
}

void HttpReply::sendHeaders() {
    httpd_resp_set_status(req_, statusLine(status_));
    httpd_resp_set_type(req_, "application/json");
    server_.addCorsHeaders(req_);
}

void HttpReply::finish() {
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

bool HttpReply::sendChunk() {
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

bool HttpReply::append(const char* data, size_t len) {
    if (failed_) return false;
    // On a short heap, what is pending goes first.
    if (!growFor(pending_, pending_.size() + len) && (!sendChunk() || !growFor(pending_, len))) return false;
    pending_.append(data, len);
    return pending_.size() <= kWindow || sendChunk();
}

HttpServer::HttpServer(Api& api) : api_(api) {}

HttpServer::~HttpServer() { end(); }

HttpServer::WorkStats HttpServer::workStats() const {
    WorkStats s;
    s.queued = workQueued_.load();
    s.run = workRun_.load();
    s.refused = workRefused_.load();
    return s;
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

void HttpServer::appendCors(std::string& head) const {
    if (!corsOrigin_) return;
    head += "Access-Control-Allow-Origin: ";
    head += corsOrigin_;
    head += "\r\n";
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

    QueryText text;
    if (!readQuery(req, request, text, reply, path)) return ESP_OK;

    // Below a remote node, before anything waits on the peer: on a worker.
    // (A streamed subscription stays here, its socket kept by httpd.)
    if (!inWorker && !text.sub && api_.belowRemote(path) && dispatchRemote(req)) return ESP_OK;

    // File nodes (docs/DESIGN.md section 4.6): raw bytes both ways, before
    // the body is read, so maxRequestBody doesn't apply to uploads.
    if (text.sub && request.op != Op::Get) {
        writeError(reply, Status::BadRequest, path, "subscribe with GET");
        return ESP_OK;
    }
    if (!text.sub && (request.op == Op::Set || request.query.view == View::Value)) {
        std::unique_ptr<FileTransfer> owned(new FileTransfer(api_, FileTransfer::Remotes::Wait));
        FileTransfer& file = *owned;
        FileTransfer::Open o = request.op == Op::Set
                                   ? file.openUpload(path, request.client, req->content_len, reply, text.filename)
                                   : file.openDownload(path, request.client, reply, text.filename);
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
    Reservation bodyRoom;
    if (req->content_len > 0) {
        // Room first (docs/DESIGN.md section 15): on a short heap the client
        // hears busy, and the body is read and dropped.
        bodyRoom = memoryBudget().reserve(req->content_len + 1, 0, true, true, mallocPool(req->content_len + 1));
        if (!bodyRoom) {
            writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for the request body");
            return drain(req, req->content_len) ? ESP_OK : ESP_FAIL;
        }
        body.resize(req->content_len);
        bodyRoom.settle();
        size_t got = 0;
        while (got < body.size()) {
            int n = recvBody(req, &body[got], body.size() - got);
            if (n <= 0) return ESP_FAIL;
            got += static_cast<size_t>(n);
        }
    }
    if (!body.empty() && !text.shape.empty()) {
        writeError(reply, Status::BadRequest, path, "shape given twice");
        return ESP_OK;
    }
    const std::string& json = body.empty() ? text.shape : body;
    JsonDocument doc;
    if (!json.empty()) {
        DeserializationError err = parseJson(doc, json, requestNesting(cfg.maxDepth));
        if (err == DeserializationError::NoMemory) {
            writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for the request body");
            return ESP_OK;
        }
        if (err) {
            writeError(reply, Status::BadRequest, path, "malformed JSON body");
            return ESP_OK;
        }
        request.body = doc.as<JsonVariantConst>();
    }
    if (text.sub) return serveStream(req, request, reply);

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

}  // namespace tesser

#endif
