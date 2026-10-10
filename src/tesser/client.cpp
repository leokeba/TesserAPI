#include "tesser/client.h"

#include "tesser/api.h"
#include "tesser/envelope.h"
#include "tesser/sink.h"

namespace tesser {

// ---- inbox ------------------------------------------------------------------

void ClientInbox::response(ResponseHandler done, Status status, std::string json) {
    MutexGuard guard(mutex);
    if (closed) return;
    items.push_back(Item{false, std::move(done), status, std::move(json)});
}

bool ClientInbox::notification(std::string json) {
    MutexGuard guard(mutex);
    if (closed) return true;  // nobody to tell
    if (notifications >= limit) return false;
    notifications++;
    items.push_back(Item{true, nullptr, Status::Ok, std::move(json)});
    return true;
}

// ---- ApiClient --------------------------------------------------------------

ApiClient::ApiClient(size_t maxQueued) : inbox_(std::make_shared<ClientInbox>(maxQueued)) {}

ApiClient::~ApiClient() {
    MutexGuard guard(inbox_->mutex);
    inbox_->closed = true;
    inbox_->items.clear();
}

size_t ApiClient::process() {
    std::vector<ClientInbox::Item> items;
    {
        MutexGuard guard(inbox_->mutex);
        items.swap(inbox_->items);
        inbox_->notifications = 0;
    }
    for (ClientInbox::Item& item : items) {
        JsonDocument doc;
        bool parsed = item.json.empty() || !deserializeJson(doc, item.json, DeserializationOption::NestingLimit(kWrittenNesting));
        if (item.notification) {
            if (onNotification_ && parsed && doc.is<JsonObjectConst>()) onNotification_(doc.as<JsonObjectConst>());
        } else if (item.done) {
            if (!parsed) doc.clear();  // out of memory: no half a body
            item.done(parsed ? item.status : Status::Internal, doc.as<JsonVariantConst>());
        }
    }
    return items.size();
}

size_t ApiClient::queued() const {
    MutexGuard guard(inbox_->mutex);
    return inbox_->items.size();
}

// ---- LocalClient ------------------------------------------------------------

namespace {

// Captures a response and queues it for process(). Detaching (deferred
// actions, queued mode, remote nodes) hands the handler to a heap copy.
class LocalReply : public Reply {
public:
    LocalReply(std::shared_ptr<ClientInbox> inbox, ApiClient::ResponseHandler done)
        : inbox_(std::move(inbox)), done_(std::move(done)) {}

    Sink& begin(Status s) override {
        status_ = s;
        body_.clear();
        return sink_;
    }
    void end() override { inbox_->response(std::move(done_), status_, std::move(body_)); }
    bool rollback() override {
        body_.clear();
        return true;
    }
    Reply* detach() override { return new LocalReply(inbox_, std::move(done_)); }
    void release() override { delete this; }

private:
    std::shared_ptr<ClientInbox> inbox_;
    ApiClient::ResponseHandler done_;
    Status status_ = Status::Internal;
    std::string body_;
    StringSink sink_{body_};
};

}  // namespace

LocalClient::LocalClient(Api& api, size_t maxQueued) : ApiClient(maxQueued), api_(api) {}

LocalClient::~LocalClient() { api_.dropSubscriber(this); }

bool LocalClient::request(Op op, std::string_view path, std::string_view bodyJson, ResponseHandler done,
                          const Query& query) {
    JsonDocument doc;
    if (!bodyJson.empty() && deserializeJson(doc, bodyJson.data(), bodyJson.size(),
                                             DeserializationOption::NestingLimit(requestNesting(api_.config().maxDepth)))) {
        return false;
    }
    Request req;
    req.op = op;
    req.path = path;
    req.query = query;
    req.body = doc.as<JsonVariantConst>();
    req.client.transport = TransportKind::Local;
    req.client.authenticated = authenticated_;
    req.subscriber = this;
    LocalReply reply(inbox_, std::move(done));
    api_.handle(req, reply);
    return true;
}

bool LocalClient::notify(const std::string& message, Delivery) { return inbox_->notification(message); }

// ---- PeerClient -------------------------------------------------------------

PeerClient::PeerClient(DatagramEndpoint& endpoint, const PeerAddress& peer, size_t maxQueued)
    : ApiClient(maxQueued), endpoint_(endpoint), peer_(peer), subscribed_(std::make_shared<std::vector<std::string>>()) {
    endpoint_.addClient(peer_, inbox_);
}

PeerClient::~PeerClient() {
    endpoint_.removeClient(inbox_.get());
    endpoint_.cancelCalls(this);
    std::vector<std::string> paths;
    {
        MutexGuard guard(inbox_->mutex);
        paths.swap(*subscribed_);
    }
    for (const std::string& p : paths) {
        endpoint_.request(peer_, Op::Unsubscribe, p, std::string_view(), [](Status, JsonVariantConst) {});
    }
}

bool PeerClient::request(Op op, std::string_view path, std::string_view bodyJson, ResponseHandler done,
                         const Query& query) {
    std::shared_ptr<ClientInbox> inbox = inbox_;
    std::shared_ptr<std::vector<std::string>> subscribed = subscribed_;
    std::string p(path.empty() ? std::string_view("/") : path);
    auto queue = [inbox, subscribed, done = std::move(done), op, p](Status s, JsonVariantConst body) mutable {
        if (s == Status::Ok && (op == Op::Subscribe || op == Op::Unsubscribe)) {
            // Remembered so the destructor can unsubscribe.
            MutexGuard guard(inbox->mutex);
            std::vector<std::string>& paths = *subscribed;
            bool known = false;
            for (size_t i = 0; i < paths.size(); i++) {
                if (paths[i] != p) continue;
                known = true;
                if (op == Op::Unsubscribe) paths.erase(paths.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
            if (!known && op == Op::Subscribe) paths.push_back(p);
        }
        std::string json;
        if (!body.isNull()) serializeJson(body, json);
        inbox->response(std::move(done), s, std::move(json));
    };
    return endpoint_.request(peer_, op, path, bodyJson, std::move(queue), query, 0, this);
}

}  // namespace tesser
