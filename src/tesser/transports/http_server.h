#pragma once

// HTTP transport on esp_http_server (ESP-IDF, and Arduino-ESP32 3.x, which
// ships it). See docs/DESIGN.md section 10.2.
#if defined(ESP_PLATFORM)

#include <stdint.h>

#include <string>

#include "esp_http_server.h"
#include "tesser/api.h"

namespace tesser {

class HttpServer {
public:
    explicit HttpServer(Api& api);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Starts a dedicated server (wildcard URI matching, 6 KB stack) and
    // serves the API under basePath ("/api" → GET /api/lamp/on).
    esp_err_t begin(uint16_t port = 80, const char* basePath = "/api");
    // Same, with a custom configuration. uri_match_fn is forced to
    // httpd_uri_match_wildcard.
    esp_err_t begin(httpd_config_t config, const char* basePath = "/api");
    // Registers on an existing server, e.g. one shared with a web UI or
    // PsychicHttp. That server must use httpd_uri_match_wildcard and should
    // have a stack of at least 6 KB. Uses 4 URI handler slots (5 with CORS).
    esp_err_t attach(httpd_handle_t server, const char* basePath = "/api");
    // Unregisters the handlers, and stops the server if begin() started it.
    void end();

    // Adds Access-Control-Allow-* headers and answers OPTIONS preflights.
    // Call before begin()/attach().
    void enableCors(const char* origin = "*") { corsOrigin_ = origin; }

    httpd_handle_t handle() const { return server_; }
    Api& api() { return api_; }

private:
    static esp_err_t onRequest(httpd_req_t* req);
    static esp_err_t onOptions(httpd_req_t* req);
    esp_err_t serve(httpd_req_t* req);
    void addCorsHeaders(httpd_req_t* req);

    Api& api_;
    httpd_handle_t server_ = nullptr;
    bool ownsServer_ = false;
    std::string base_;
    std::string pattern_;
    const char* corsOrigin_ = nullptr;
    friend class HttpReply;
};

}  // namespace tesser

#endif
