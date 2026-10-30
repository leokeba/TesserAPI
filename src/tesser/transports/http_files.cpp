#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

#include <stdio.h>

// File nodes on HttpServer: raw bytes both ways (docs/DESIGN.md section 10.2).

namespace tesser {

using namespace http;

esp_err_t HttpServer::receiveFile(httpd_req_t* req, FileTransfer& file, Reply& reply) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[kFileChunk]);
    size_t left = req->content_len;
    while (left) {
        int n = recvBody(req, reinterpret_cast<char*>(buf.get()), left < kFileChunk ? left : kFileChunk);
        if (n <= 0) return ESP_FAIL;  // the client went away: the transfer is aborted
        left -= static_cast<size_t>(n);
        if (!file.write(buf.get(), static_cast<size_t>(n), reply)) {
            return left <= kDrainLimit && drain(req, left) ? ESP_OK : ESP_FAIL;
        }
    }
    file.finish(reply);
    return ESP_OK;
}

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
    appendCors(head);
    if (corsOrigin_) head += "Access-Control-Expose-Headers: Content-Disposition, Content-Length\r\n";
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
