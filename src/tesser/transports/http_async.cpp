#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

#include "esp_idf_version.h"
#include "esp_timer.h"

// What outlives a request's handler on HttpServer: deferred replies answered
// later, and requests below remote nodes on tasks of their own
// (docs/DESIGN.md sections 10.2 and 14).

namespace tesser {

using namespace http;

namespace {

class DeferredHttpReply : public Reply {
public:
    using DeferredState = HttpServer::DeferredState;
    DeferredHttpReply(DeferredState* st, size_t limit) : st_(st), sink_(st->body, limit, true) {}
    Sink& begin(Status s) override {
        // Timed out already: nobody reads it.
        if (st_->answered.load()) return discard_;
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
    CountingSink discard_;
};

}  // namespace

Reply* HttpReply::detach() {
    if (deferred_) return nullptr;
    deferred_ = new DeferredState();
    if (!deferred_->done) {
        delete deferred_;
        deferred_ = nullptr;
        return nullptr;
    }
    return new DeferredHttpReply(deferred_, server_.api().config().maxResponse);
}

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
        st->timeoutBody = std::move(r.body);
    }
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    auto work = [](void* arg) {
        auto* s = static_cast<DeferredState*>(arg);
        bool late = !s->timeoutBody.empty();  // written only on a timeout, before this was queued
        const std::string& body = late ? s->timeoutBody : s->body;
        httpd_resp_set_status(s->async, statusLine(late ? Status::Timeout : s->status));
        httpd_resp_set_type(s->async, "application/json");
        s->server->addCorsHeaders(s->async);
        httpd_resp_send(s->async, body.data(), static_cast<ssize_t>(body.size()));
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

}  // namespace tesser

#endif
