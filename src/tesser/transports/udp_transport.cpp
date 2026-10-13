#include "tesser/transports/udp_transport.h"

#if defined(ESP_PLATFORM)

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

namespace tesser {

namespace {

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

PeerAddress fromSockaddr(const sockaddr_in& a) {
    uint8_t b[6];
    memcpy(b, &a.sin_addr.s_addr, 4);  // network order: a.b.c.d
    uint16_t port = ntohs(a.sin_port);
    b[4] = static_cast<uint8_t>(port >> 8);
    b[5] = static_cast<uint8_t>(port);
    return PeerAddress::fromBytes(b, 6);
}

sockaddr_in toSockaddr(const PeerAddress& p) {
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr.s_addr, p.bytes, 4);
    a.sin_port = htons(static_cast<uint16_t>(p.bytes[4] << 8 | p.bytes[5]));
    return a;
}

constexpr size_t kMaxInterfaces = 6;

struct Interfaces {
    esp_netif_t* list[kMaxInterfaces];
    size_t count = 0;
};

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
bool collect(esp_netif_t* netif, void* ctx) {
    auto* found = static_cast<Interfaces*>(ctx);
    if (found->count < kMaxInterfaces) found->list[found->count++] = netif;
    return false;  // visit them all
}
#endif

}  // namespace

UdpTransport::UdpTransport(Api* api, UdpLink::Options options, DatagramEndpoint::Options endpointOptions)
    : link_(
          api, [this](const PeerAddress& to, const uint8_t* data, size_t len) { return sendTo(to, data, len); },
          &nowMs, options, endpointOptions) {}

UdpTransport::~UdpTransport() {
    end();
    while (workTask_) vTaskDelay(pdMS_TO_TICKS(5));
}

std::vector<PeerAddress> UdpTransport::broadcastAddresses(uint16_t port) {
    Interfaces found;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
    esp_netif_find_if(&collect, &found);
#else
    for (esp_netif_t* n = esp_netif_next(nullptr); n && found.count < kMaxInterfaces; n = esp_netif_next(n)) {
        found.list[found.count++] = n;
    }
#endif
    std::vector<PeerAddress> out;
    for (size_t i = 0; i < found.count; i++) {
        esp_netif_ip_info_t info;
        if (!esp_netif_is_netif_up(found.list[i]) || esp_netif_get_ip_info(found.list[i], &info) != ESP_OK ||
            info.ip.addr == 0) {
            continue;
        }
        // Both in network order, so the bits line up either way.
        uint32_t b = info.ip.addr | ~info.netmask.addr;
        uint8_t ip[4];
        memcpy(ip, &b, 4);
        PeerAddress a = udpAddress(ip[0], ip[1], ip[2], ip[3], port);
        bool again = false;
        for (const PeerAddress& o : out) again = again || o == a;
        if (!again) out.push_back(a);
    }
    return out;
}

bool UdpTransport::sendTo(const PeerAddress& to, const uint8_t* data, size_t len) {
    std::vector<PeerAddress> targets;
    if (to.length == 0) {
        targets = broadcastAddresses(port_);
    } else if (to.length == 6) {
        targets.push_back(to);
    }
    bool ok = !targets.empty();
    MutexGuard guard(socketMutex_);
    int s = socket_.load();
    if (s < 0) return false;
    for (const PeerAddress& t : targets) {
        sockaddr_in a = toSockaddr(t);
        bool sent = false;
        // Wi-Fi refuses bursts with ENOMEM while its buffers are full.
        for (int attempt = 0; attempt < 20 && !sent; attempt++) {
            sent = sendto(s, data, len, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) >= 0;
            if (!sent && errno != ENOMEM && errno != EAGAIN) break;
            if (!sent) vTaskDelay(1);
        }
        ok = ok && sent;
    }
    return ok;
}

bool UdpTransport::begin(uint16_t port, bool runTask, uint32_t stackSize, UBaseType_t priority) {
    MutexGuard taskGuard(taskMutex_);
    if (running_) return false;
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        close(s);
        return false;
    }
    // The receive task checks for end() this often.
    timeval tv = {0, 100000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    buffer_.resize(link_.options().fragmentSize + 20u > 1472u ? link_.options().fragmentSize + 20u : 1472u);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    link_.setDeviceId(mac);
    if (!named_) {
        char name[16];
        snprintf(name, sizeof(name), "tesser-%02x%02x", mac[4], mac[5]);
        link_.setName(name);
    }
    port_ = port;
    socket_ = s;
    running_ = true;
    link_.start(esp_random());
    if (!runTask) {
        link_.onQueued(nullptr);
        return true;
    }
    // A worker that end() let finish may still be there: it carries on.
    if (!workTask_ && xTaskCreate(&UdpTransport::workEntry, "tesser_udp", stackSize, this, priority, &workTask_) !=
                          pdPASS) {
        workTask_ = nullptr;
    }
    if (workTask_) {
        TaskHandle_t task = workTask_;
        link_.onQueued([task] { xTaskNotifyGive(task); });
    }
    if (!workTask_ ||
        xTaskCreate(&UdpTransport::receiveEntry, "tesser_udp_rx", 4096, this, priority + 1, &receiveTask_) != pdPASS) {
        receiveTask_ = nullptr;
        running_ = false;
        link_.stop();
        MutexGuard guard(socketMutex_);
        close(s);
        socket_ = -1;
        return false;
    }
    return true;
}

void UdpTransport::end() {
    if (!running_) return;
    link_.stop();  // the goodbye goes out before the socket closes
    running_ = false;
    while (receiveTask_) vTaskDelay(pdMS_TO_TICKS(10));
    MutexGuard guard(socketMutex_);
    int s = socket_.exchange(-1);
    if (s >= 0) close(s);
    // The worker exits on its own after one more tick, which forgets the
    // peers; waiting for it here could deadlock a caller holding the API.
}

void UdpTransport::receiveWaiting() {
    int s = socket_.load();
    if (s < 0) return;
    for (;;) {
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(s, buffer_.data(), buffer_.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from),
                         &fromLen);
        if (n <= 0) return;
        link_.packet(fromSockaddr(from), buffer_.data(), static_cast<size_t>(n));
    }
}

void UdpTransport::poll() {
    if (running_) receiveWaiting();
    link_.tick();
}

void UdpTransport::receiveEntry(void* arg) {
    auto* self = static_cast<UdpTransport*>(arg);
    int s = self->socket_.load();
    while (self->running_) {
        sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        int n = recvfrom(s, self->buffer_.data(), self->buffer_.size(), 0, reinterpret_cast<sockaddr*>(&from),
                         &fromLen);
        if (n > 0) {
            self->link_.packet(fromSockaddr(from), self->buffer_.data(), static_cast<size_t>(n));
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    self->receiveTask_ = nullptr;
    vTaskDelete(nullptr);
}

void UdpTransport::workEntry(void* arg) {
    auto* self = static_cast<UdpTransport*>(arg);
    for (;;) {
        // Wake on new messages; tick for retransmissions and timeouts.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        self->link_.tick();
        if (self->running_) continue;
        self->link_.tick();  // the peers end() forgot
        MutexGuard guard(self->taskMutex_);
        if (!self->running_) {
            self->workTask_ = nullptr;
            break;
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace tesser

#endif
