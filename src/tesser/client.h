#pragma once

#include <stddef.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/datagram.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;

// Responses and notifications waiting for ApiClient::process(). Shared with
// the replies and the endpoint that fill it, so they can outlive the client.
struct ClientInbox {
    using ResponseHandler = std::function<void(Status status, JsonVariantConst body)>;
    struct Item {
        bool notification;
        ResponseHandler done;
        Status status;
        std::string json;
    };

    explicit ClientInbox(size_t maxNotifications) : limit(maxNotifications) {}
    void response(ResponseHandler done, Status status, std::string json);
    // False when `limit` notifications are already waiting.
    bool notification(std::string json);

    Mutex mutex;
    std::vector<Item> items;
    size_t limit;
    size_t notifications = 0;
    bool closed = false;  // the client is gone: drop everything
};

// A connection to one API, in this process (LocalClient) or on another node
// (PeerClient): what a user interface, or any other client, is written
// against (docs/DESIGN.md section 14.2).
//
// Responses and notifications are queued, and process() delivers them in
// the caller's task: a single-threaded UI loop calls process() and needs no
// locking. Handlers may send new requests.
class ApiClient {
public:
    using ResponseHandler = ClientInbox::ResponseHandler;
    // A "change" or "event" envelope for one of this client's subscriptions.
    using NotificationHandler = std::function<void(JsonObjectConst envelope)>;

    explicit ApiClient(size_t maxQueued);
    virtual ~ApiClient();
    ApiClient(const ApiClient&) = delete;
    ApiClient& operator=(const ApiClient&) = delete;

    // Sends a request; `done` (which may be empty) runs in process() with the
    // status and body, or with Status::Timeout. `bodyJson` is JSON text, or
    // empty. Returns false, without calling `done`, if it couldn't be sent.
    virtual bool request(Op op, std::string_view path, std::string_view bodyJson, ResponseHandler done,
                         const Query& query = Query()) = 0;

    bool get(std::string_view path, ResponseHandler done, const Query& query = Query()) {
        return request(Op::Get, path, std::string_view(), std::move(done), query);
    }
    bool set(std::string_view path, std::string_view bodyJson, ResponseHandler done = nullptr) {
        return request(Op::Set, path, bodyJson, std::move(done));
    }
    bool subscribe(std::string_view path, ResponseHandler done, const Query& query = Query()) {
        return request(Op::Subscribe, path, std::string_view(), std::move(done), query);
    }
    bool unsubscribe(std::string_view path, ResponseHandler done = nullptr) {
        return request(Op::Unsubscribe, path, std::string_view(), std::move(done));
    }

    void onNotification(NotificationHandler fn) { onNotification_ = std::move(fn); }

    // Delivers what is queued, in arrival order. Returns how many items it
    // delivered. Notifications beyond `maxQueued` are dropped; the next one
    // carries "overflow": true when the server noticed.
    size_t process();
    size_t queued() const;

protected:
    std::shared_ptr<ClientInbox> inbox_;

private:
    NotificationHandler onNotification_;
};

// A client of an API on this chip, as if it were remote: requests go through
// Api::handle() like a transport's, and subscriptions work. A UI running in
// the same firmware uses it, so it is written once for local and remote
// nodes.
class LocalClient : public ApiClient, public Subscriber {
public:
    explicit LocalClient(Api& api, size_t maxQueued = 16);
    ~LocalClient() override;

    bool request(Op op, std::string_view path, std::string_view bodyJson, ResponseHandler done,
                 const Query& query = Query()) override;

    // Requests count as authenticated (Api::authorize()) unless set to false.
    void setAuthenticated(bool authenticated) { authenticated_ = authenticated; }

    // Subscriber
    bool notify(const std::string& message, Delivery delivery) override;

private:
    Api& api_;
    bool authenticated_ = true;
};

// A client of another node's API over a DatagramEndpoint (NowTP, ...). Its
// subscriptions are undone when it is destroyed.
class PeerClient : public ApiClient {
public:
    PeerClient(DatagramEndpoint& endpoint, const PeerAddress& peer, size_t maxQueued = 16);
    ~PeerClient() override;

    bool request(Op op, std::string_view path, std::string_view bodyJson, ResponseHandler done,
                 const Query& query = Query()) override;
    const PeerAddress& peer() const { return peer_; }

private:
    DatagramEndpoint& endpoint_;
    PeerAddress peer_;
    std::shared_ptr<std::vector<std::string>> subscribed_;  // paths, guarded by the inbox mutex
};

}  // namespace tesser
