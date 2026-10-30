#pragma once

// What the parts of HttpServer share (docs/DESIGN.md section 16): one server,
// in a file per concern, http_server.cpp holding its life and the dispatch of
// requests.

#if defined(ESP_PLATFORM)

#include <stdint.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/file_session.h"
#include "tesser/http_checks.h"
#include "tesser/stream_subscriber.h"
#include "tesser/transports/http_server.h"

namespace tesser {

namespace http {

constexpr size_t kChunk = 512;
constexpr size_t kFileChunk = 2048;  // file transfers, on the heap
// A refused upload's body is read and dropped up to this size, so the client
// (which sends it all before reading) gets the error; beyond it, the
// connection is closed.
constexpr size_t kDrainLimit = 256 * 1024;

// The stack copies what is sent (TCP's segments, then the radio's buffers)
// and holds it until the client acknowledges it: about twice it. A large
// piece waits, up to 500 ms, for the heap to hold that above the floor,
// while what went before is acknowledged (docs/DESIGN.md section 15).
// Measured on DEVKIT-V3: a 3.9 KB WebSocket frame took about 6.5 KB to
// send, and a 100 KB file download held 4-6 KB free for 2.5 s, 2 KB
// pieces sent as fast as the socket took them.
inline void waitForRoom(size_t bytes) {
    for (int i = 0; bytes > 1024 && i < 50 && !heapAbove(2 * bytes); i++) vTaskDelay(pdMS_TO_TICKS(10));
}

// ---- http_request.cpp: reading a request ---------------------------------------
// Reads what there is of a request's body, up to `len`: a client that stalls
// is given up on after a few socket timeouts (0 or less: closed, or given up
// on), or it would hold the httpd task, and every other client, for good.
int recvBody(httpd_req_t* req, char* buf, size_t len);
// Reads and drops `left` bytes of request body. False if the client went away.
bool drain(httpd_req_t* req, size_t left);
bool sendAll(httpd_req_t* req, const char* data, size_t len);
const char* statusLine(Status s);
// What a request's query says beyond Query's members, which point into it.
struct QueryText {
    std::string keys, exclude, shape, filename, eventHint;
    bool sub = false;
};
// Reads the query string into `request`: false, with the error answered, if
// it can't. `query` must outlive the request.
bool readQuery(httpd_req_t* req, Request& request, QueryText& query, Reply& reply, std::string_view path);
// A request header's value; empty when it is absent.
std::string header(httpd_req_t* req, const char* name);
// The token a request presents ("Authorization: Bearer <token>" or
// ?token=<token>), empty if none or too long.
std::string requestToken(httpd_req_t* req);
// The address of the client on socket `fd`.
void peerAddress(int fd, Client& client);
// A refusal of the strict mode, with an error body like any other.
void refuse(httpd_req_t* req, const char* statusLine, Status status, Reason reason, const char* message,
            const char* corsOrigin);

}  // namespace http

// Shared between a parked HTTP handler and the deferred reply that completes
// it. Whoever lets go last frees it. With async handlers, the handler doesn't
// park: the request is answered later, from the httpd task.
struct HttpServer::DeferredState {
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    std::atomic<int> refs{2};
    Status status = Status::Internal;
    std::string body;  // the reply's: written by it, read once it ended
    // The timeout's answer, apart: a reply arriving at the same moment may
    // still be writing `body` from another task.
    std::string timeoutBody;
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

// Streams the body: what is written waits, up to a window, for drain(),
// which the core calls without the API lock (docs/DESIGN.md section 9);
// beyond the window it goes at once. A response that fits the window when it
// ends goes out whole with a Content-Length; a larger one is chunked.
class HttpReply : public Reply {
public:
    using DeferredState = HttpServer::DeferredState;
    static constexpr size_t kWindow = 2048;

    HttpReply(HttpServer& server, httpd_req_t* req) : server_(server), req_(req) {}
    ~HttpReply() override;

    Sink& begin(Status s) override;
    void end() override;
    bool rollback() override;
    bool wantsDrain() const override { return !failed_ && (ended_ || pending_.size() >= http::kChunk); }
    void drain() override;
    Reply* detach() override;  // http_async.cpp

    DeferredState* deferred() const { return deferred_; }

    // Sends a complete response produced elsewhere (deferred replies).
    void sendWhole(Status s, const std::string& body);

private:
    class ChunkSink : public Sink {
    public:
        explicit ChunkSink(HttpReply& r) : r_(r) {}
        bool write(const char* data, size_t len) override { return r_.append(data, len); }

    private:
        HttpReply& r_;
    };

    void sendHeaders();
    void finish();
    bool sendChunk();
    bool append(const char* data, size_t len);

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
    bool notifyOwned(std::string&& message, Delivery) override { return server_.wsSend(fd, std::move(message)); }
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
          subscriber(owner.api_, [this](const StreamSubscriber::Piece& piece) {
              HttpServer* s = this->server.load();
              return s && s->streamSend(this, piece);
          }) {}

    std::atomic<HttpServer*> server;  // null once the server ended
    httpd_handle_t handle;
    int fd;
    StreamSubscriber subscriber;
};

}  // namespace tesser

#endif
