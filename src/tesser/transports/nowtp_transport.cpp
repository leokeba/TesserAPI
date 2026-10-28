#include "tesser/transports/nowtp_transport.h"

#if defined(ESP_PLATFORM) && defined(TESSER_HAVE_NOWTP)

#include <stdio.h>
#include <stdlib.h>

#include <type_traits>
#include <utility>

#include "esp_timer.h"
#include "tesser/api.h"

namespace tesser {

namespace {

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// NowTP 8320a40 and later let a handler take a reassembled message's
// malloc'd buffer (Message::buffer); older ones only lend `data`.
template <class M, class = void>
struct HasBuffer : std::false_type {};
template <class M>
struct HasBuffer<M, decltype(void(std::declval<const M&>().buffer))> : std::true_type {};

template <class M>
uint8_t* takeBuffer(const M& m) {
    if constexpr (HasBuffer<M>::value) {
        uint8_t* b = m.buffer;
        m.buffer = nullptr;
        return b;
    } else {
        (void)m;
        return nullptr;
    }
}

// NowTP f09a571 and later let a handler refuse a message it can't take
// (Message::refused): a reliable sender is told busy and sends it again.
template <class M, class = void>
struct HasRefused : std::false_type {};
template <class M>
struct HasRefused<M, decltype(void(std::declval<const M&>().refused))> : std::true_type {};

template <class M>
void refuse(const M& m) {
    if constexpr (HasRefused<M>::value) {
        m.refused = true;
    } else {
        (void)m;
    }
}

// NowTP ce6b645 and later send from a malloc()ed buffer they take over
// (sendBuffer()); older ones only copy.
template <class T, class = void>
struct HasSendBuffer : std::false_type {};
template <class T>
struct HasSendBuffer<T, decltype(void(std::declval<T&>().sendBuffer(
                            std::declval<const nowtp::Mac&>(), uint8_t(0), static_cast<uint8_t*>(nullptr), size_t(0),
                            size_t(0), std::declval<const nowtp::SendOptions&>(), nowtp::CompletionHandler())))>
    : std::true_type {};

// Room NowTP wants after the payload to send it in place (its
// kSendOverhead: 30 bytes, headroom included, as of v0.7.2); less, and it
// reallocates.
constexpr size_t kSendSlack = 32;

// NowTP deadc54 and later know each peer's largest message, from its
// discovery (peerMaxMessageSize(), 0 while unknown).
template <class T, class = void>
struct HasPeerLimit : std::false_type {};
template <class T>
struct HasPeerLimit<T, decltype(void(std::declval<const T&>().peerMaxMessageSize(std::declval<const nowtp::Mac&>())))>
    : std::true_type {};

template <class T>
void installPeerLimit(DatagramEndpoint& endpoint, T& now) {
    if constexpr (HasPeerLimit<T>::value) {
        endpoint.setPeerLimit([&now](const PeerAddress& peer) {
            return peer.length == 6 ? now.peerMaxMessageSize(nowtp::Mac::from(peer.bytes)) : size_t(0);
        });
    } else {
        (void)endpoint;
        (void)now;
    }
}

// NowTP v0.7.2 and later report outcomes to one observer per port, with a
// tag set per send (SendOptions::tag), in place of a handler per send.
template <class T, class = void>
struct HasObserver : std::false_type {};
template <class T>
struct HasObserver<T, decltype(void(std::declval<T&>().observeCompletions(uint8_t(0), nullptr)))> : std::true_type {};

// What a NowTP outcome means for the endpoint; false when nothing failed.
bool failureOf(nowtp::Status s, SendFailure& f) {
    if (s == nowtp::Status::SendFailed || s == nowtp::Status::Timeout) {
        f = SendFailure::Undelivered;
    } else if (s == nowtp::Status::Rejected) {
        f = SendFailure::Rejected;
    } else if (s == nowtp::Status::AuthFailed) {
        f = SendFailure::AuthFailed;
    } else {
        return false;  // delivered, superseded, cancelled
    }
    return true;
}

constexpr uint32_t kTagIdBits = 0x00ffffffu;

}  // namespace


template <class T>
void NowTpTransport::installObserver(T& now) {
    if constexpr (HasObserver<T>::value) {
        // Runs in NowTP's task: it only queues.
        now.observeCompletions(port_, [reports = reports_](uint32_t tag, nowtp::Status s) {
            SendFailure f;
            if (!failureOf(s, f)) return;
            MutexGuard guard(reports->mutex);
            size_t peer = tag >> 24;
            if (!reports->endpoint || !peer || peer > reports->peers.size()) return;
            reports->endpoint->sendFailed(reports->peers[peer - 1], tag & kTagIdBits, f, kTagIdBits);
        });
    } else {
        (void)now;
    }
}

template <class O>
void NowTpTransport::reportTo(const PeerAddress& to, uint32_t callId, O& opts, nowtp::CompletionHandler& done) {
    if constexpr (HasObserver<nowtp::EspNowTransport>::value) {
        MutexGuard guard(reports_->mutex);
        std::vector<PeerAddress>& peers = reports_->peers;
        size_t i = 0;
        while (i < peers.size() && peers[i] != to) i++;
        if (i == peers.size() && i < 255) peers.push_back(to);
        if (i < peers.size()) {
            opts.tag = static_cast<uint32_t>(i + 1) << 24 | (callId & kTagIdBits);
            return;
        }
    }
    done = completion(to, callId);
}

nowtp::CompletionHandler NowTpTransport::completion(const PeerAddress& to, uint32_t callId) {
    // A delivery that fails later (docs/DESIGN.md section 10.3): the
    // endpoint fails its calls at once rather than letting them time out.
    // Runs in NowTP's task: it only queues.
    return [reports = reports_, to, callId](nowtp::Status s) {
        SendFailure f;
        if (!failureOf(s, f)) return;
        MutexGuard guard(reports->mutex);
        if (reports->endpoint) reports->endpoint->sendFailed(to, callId, f);
    };
}

template <class T>
void NowTpTransport::installOwnedSend(T& now) {
    if constexpr (HasSendBuffer<T>::value) {
        // Replies are built in the buffer NowTP sends from: on a short
        // heap, a large one isn't held twice (docs/DESIGN.md section 15).
        endpoint_.setOwnedSend(
            [this, &now](const PeerAddress& to, Bytes message, Delivery delivery) {
                if (!message.data()) return false;
                nowtp::Mac mac = nowtp::Mac::from(to.bytes);
                nowtp::SendOptions opts;
                opts.reliable = delivery == Delivery::Reliable && !mac.isBroadcast();
                opts.latestOnly = delivery == Delivery::LatestOnly;
                // The call's id read before the buffer goes to NowTP: a
                // relayed part it refuses fails at once (docs/DESIGN.md
                // section 10.3), not after its timeouts.
                nowtp::CompletionHandler done;
                if (opts.reliable) {
                    reportTo(to, DatagramEndpoint::callIdOf(std::string_view(message.data(), message.size())), opts,
                             done);
                }
                size_t len = message.size(), capacity = message.capacity();
                auto* buffer = reinterpret_cast<uint8_t*>(message.release());
                return now.sendBuffer(mac, port_, buffer, len, capacity, opts, std::move(done)) == nowtp::Status::Ok;
            },
            kSendSlack);
    } else {
        (void)now;
    }
}

NowTpTransport::NowTpTransport(Api* api, nowtp::EspNowTransport& now, uint8_t port,
                               DatagramEndpoint::Options options)
    : now_(now),
      port_(port),
      endpoint_(
          api, TransportKind::NowTP,
          [this](const PeerAddress& to, const std::string& message, Delivery delivery) {
              nowtp::Mac mac = nowtp::Mac::from(to.bytes);
              nowtp::SendOptions opts;
              opts.reliable = delivery == Delivery::Reliable && !mac.isBroadcast();
              opts.latestOnly = delivery == Delivery::LatestOnly;
              nowtp::CompletionHandler done;
              if (opts.reliable) reportTo(to, DatagramEndpoint::callIdOf(message), opts, done);
              // QueueFull (per destination) is a full buffer, not a failure:
              // false here, and the sender tries again or answers busy.
              return now_.send(mac, port_, message.data(), message.size(), opts, std::move(done)) ==
                     nowtp::Status::Ok;
          },
          &nowMs, options) {
    reports_->endpoint = &endpoint_;
    installObserver(now_);
    installOwnedSend(now_);
    installPeerLimit(endpoint_, now_);
}

NowTpTransport::~NowTpTransport() {
    {
        MutexGuard guard(reports_->mutex);
        reports_->endpoint = nullptr;
    }
    end();
}

bool NowTpTransport::begin(bool runTask, uint32_t stackSize, UBaseType_t priority) {
    if (running_) return false;
    running_ = true;
    if (runTask) {
        if (xTaskCreate(&NowTpTransport::taskEntry, "tesser_nowtp", stackSize, this, priority, &task_) != pdPASS) {
            running_ = false;
            return false;
        }
        TaskHandle_t task = task_;
        endpoint_.onQueued([task] { xTaskNotifyGive(task); });
        ownHook_ = true;
    }
    // A message the endpoint can't take now (the heap floor, its queue) is
    // refused, so the sender sends it again, rather than answered busy
    // (docs/DESIGN.md section 15).
    endpoint_.linkRefuses(HasRefused<nowtp::Message>::value);
    now_.listen(port_, [this](const nowtp::Message& m) {
        // A reassembled message is kept as NowTP built it: a large reply
        // isn't on a short heap twice. A single frame's data is copied.
        bool taken;
        if (uint8_t* buffer = takeBuffer(m)) {
            taken = endpoint_.receiveOwned(address(m.src), buffer, m.len, m.reliable);
        } else {
            taken = endpoint_.receive(address(m.src), reinterpret_cast<const char*>(m.data), m.len, m.reliable);
        }
        if (!taken) refuse(m);
    });
    return true;
}

void NowTpTransport::end() {
    if (!running_) return;
    now_.listen(port_, nullptr);
    // As UDP's end(): every peer is forgotten (calls fail, subscriptions and
    // transfers are dropped, remote nodes show offline and go by another
    // route), in the worker's last process(), or the application's next.
    endpoint_.forgetAll(PeerLoss::Stopped);
    running_ = false;
    // From a request the worker handles, it finishes on its own.
    if (task_ && task_ != xTaskGetCurrentTaskHandle()) {
        xTaskNotifyGive(task_);
        while (task_) vTaskDelay(pdMS_TO_TICKS(5));
    }
    // Only our worker's hook: without a task, the application's stays.
    if (ownHook_) endpoint_.onQueued(nullptr);
    ownHook_ = false;
}

void NowTpTransport::taskEntry(void* arg) {
    auto* self = static_cast<NowTpTransport*>(arg);
    while (self->running_) {
        // Wake on new messages; poll periodically to expire client calls.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        self->endpoint_.process();
    }
    self->endpoint_.process();  // the peers end() forgot
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

bool NowTpTransport::get(const nowtp::Mac& to, std::string_view path, DatagramEndpoint::ResponseHandler done,
                         const Query& query, uint32_t timeoutMs) {
    return endpoint_.request(address(to), Op::Get, path, std::string_view(), std::move(done), query, timeoutMs);
}

bool NowTpTransport::set(const nowtp::Mac& to, std::string_view path, std::string_view bodyJson,
                         DatagramEndpoint::ResponseHandler done, uint32_t timeoutMs) {
    return endpoint_.request(address(to), Op::Set, path, bodyJson, std::move(done), Query(), timeoutMs);
}

bool NowTpTransport::advertise() {
    Api* api = endpoint_.api();
    if (!api) return false;
    uint32_t hash = api->schemaHash();
    std::string meta = buildAdvertisement(port_, hash, endpoint_.maxMessage());
    bool ok = now_.setDiscoveryMetadata(meta.data(), meta.size()) == nowtp::Status::Ok;
    // From now on the worker (or poll()) re-advertises when the tree changes.
    endpoint_.autoAdvertise(
        [this](uint32_t h) {
            std::string m = buildAdvertisement(port_, h, endpoint_.maxMessage());
            now_.setDiscoveryMetadata(m.data(), m.size());
        },
        hash);
    return ok;
}

bool NowTpTransport::parseAdvertisement(const std::vector<uint8_t>& metadata, uint8_t& port, uint32_t& schemaHash) {
    return tesser::parseAdvertisement(
        std::string_view(reinterpret_cast<const char*>(metadata.data()), metadata.size()), port, schemaHash);
}

void NowTpTransport::peerEvent(nowtp::PeerEvent event, const nowtp::PeerInfo& peer) {
    if (event == nowtp::PeerEvent::Lost) return peerLost(peer.mac);
    uint8_t port, protocol;
    uint32_t hash;
    size_t maxMessage;
    std::string_view meta(reinterpret_cast<const char*>(peer.metadata.data()), peer.metadata.size());
    if (tesser::parseAdvertisement(meta, port, hash, protocol, maxMessage) && port == port_) {
        endpoint_.peerSeen(address(peer.mac), peer.name, hash, PeerAddress(), protocol, maxMessage);
    }
}

void NowTpTransport::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    endpoint_.mountPeers(parent, mirrorIntervalMs);
    for (const nowtp::PeerInfo& p : now_.peers()) peerEvent(nowtp::PeerEvent::Found, p);
}

}  // namespace tesser

#endif
