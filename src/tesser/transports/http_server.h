#pragma once

// HTTP transport on esp_http_server (ESP-IDF, and Arduino-ESP32 3.x, which
// ships it). See docs/DESIGN.md section 10.2.
#if defined(ESP_PLATFORM)

#include <stdint.h>

#include <string>
#include <vector>

#include "esp_http_server.h"
#include "tesser/api.h"

struct esp_timer;  // esp_timer.h, kept out of applications' include path

namespace tesser {

class FileTransfer;

class HttpServer {
public:
    explicit HttpServer(Api& api);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts a dedicated server (wildcard URI matching, 8 KB stack) and
    // serves the API under basePath ("/api" → GET /api/lamp/on).
    esp_err_t begin(uint16_t port = 80, const char* basePath = "/api");
    // Same, with a custom configuration. uri_match_fn is forced to
    // httpd_uri_match_wildcard.
    esp_err_t begin(httpd_config_t config, const char* basePath = "/api");
    // Registers on an existing server, e.g. one shared with a web UI or
    // PsychicHttp. That server must use httpd_uri_match_wildcard and should
    // have a stack of at least 8 KB. Uses 4 URI handler slots (5 with CORS).
    esp_err_t attach(httpd_handle_t server, const char* basePath = "/api");
    // Unregisters the handlers, and stops the server if begin() started it.
    void end();

    // Adds Access-Control-Allow-* headers and answers OPTIONS preflights.
    // Call before begin()/attach().
    void enableCors(const char* origin = "*") { corsOrigin_ = origin; }

    // Serves the JSON envelope protocol, including subscriptions, over
    // WebSocket at `uri` (one more handler slot). Needs
    // CONFIG_HTTPD_WS_SUPPORT (on in Arduino-ESP32 3.x; enable it in
    // menuconfig on ESP-IDF). Call before begin()/attach(). With attach(),
    // closed connections are noticed on the next send to them.
    void enableWebSocket(const char* uri = "/ws") { wsUri_ = uri; }
    size_t webSocketClients() const;

    // Streamed subscriptions (docs/DESIGN.md section 10.2): GET with ?sub
    // keeps the response open and sends one notification per line. Each
    // stream holds one of the server's sockets; past `n` streams, requests
    // get busy. A stream idle for `keepAliveMs` gets a bare "\n", so a gone
    // client is noticed. Call before begin()/attach().
    void setMaxStreams(uint8_t n) { maxStreams_ = n; }
    void setStreamKeepAlive(uint32_t ms) { keepAliveMs_ = ms; }
    size_t streams() const;

    // Requests carrying "Authorization: Bearer <token>" (or ?token=<token>,
    // for WebSocket handshakes from browsers) count as authenticated, with
    // full access; see Api::authorize(). Other tokens go to the API's token
    // check (Api::authenticate()). A WebSocket connection is authenticated
    // by its handshake; since ESP-IDF 6.1 that needs
    // CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT, which TesserAPI's component
    // selects. The string must outlive the server.
    void setToken(const char* token) { token_ = token; }

    // Announces the node on the local network with mDNS (docs/DESIGN.md
    // section 10.2): a "_tesser._tcp" service named `instance`, with the
    // API's base path and WebSocket path in its TXT record ("api", "ws"),
    // and <hostname>.local when a hostname is given. After begin() or
    // attach(). Needs the mdns component: Arduino-ESP32 ships it; on ESP-IDF,
    // add espressif/mdns to the application. ESP_ERR_NOT_SUPPORTED otherwise.
    esp_err_t advertise(const char* instance, const char* hostname = nullptr);

    httpd_handle_t handle() const { return server_; }
    Api& api() { return api_; }

private:
    class WsClient;
    class Stream;

    static esp_err_t onRequest(httpd_req_t* req);
    static esp_err_t onOptions(httpd_req_t* req);
    static esp_err_t onWebSocket(httpd_req_t* req);
    static esp_err_t onWebSocketHandshake(httpd_req_t* req);
    static void onClose(httpd_handle_t server, int fd);
    esp_err_t serve(httpd_req_t* req);
    esp_err_t receiveFile(httpd_req_t* req, FileTransfer& file, Reply& reply);
    esp_err_t sendFile(httpd_req_t* req, FileTransfer& file);
    esp_err_t serveWebSocket(httpd_req_t* req);
    void addCorsHeaders(httpd_req_t* req);
    WsClient* wsClient(int fd, bool create);
    void dropWsClient(int fd);
    bool wsSend(int fd, const std::string& message);
    Access access(httpd_req_t* req) const;
    esp_err_t serveStream(httpd_req_t* req, const Request& request, Reply& reply);
    bool streamSend(Stream* stream, const std::string& piece);
    static void freeStream(void* ctx);
    static void onKeepAlive(void* arg);

    Api& api_;
    httpd_handle_t server_ = nullptr;
    bool ownsServer_ = false;
    uint16_t port_ = 80;
    std::string base_;
    std::string pattern_;
    const char* corsOrigin_ = nullptr;
    const char* wsUri_ = nullptr;
    const char* token_ = nullptr;
    mutable Mutex wsMutex_;
    std::vector<WsClient*> wsClients_;
    bool wsAuthWarned_ = false;
    mutable Mutex streamsMutex_;  // never held while taking the API lock
    std::vector<Stream*> streams_;
    uint8_t maxStreams_ = 2;
    uint32_t keepAliveMs_ = 15000;
    struct esp_timer* keepAliveTimer_ = nullptr;
    friend class HttpReply;
};

}  // namespace tesser

#endif
