#include "tesser/transports/http_internal.h"

#if defined(ESP_PLATFORM)

// mDNS (advertise()): Arduino-ESP32 ships the component; on ESP-IDF, the
// component's CMakeLists links espressif/mdns when the application has it.
#if defined(TESSER_HAVE_MDNS) || (defined(ARDUINO) && __has_include("mdns.h"))
#define TESSER_MDNS 1
#include "mdns.h"
#else
#define TESSER_MDNS 0
#endif

namespace tesser {

esp_err_t HttpServer::advertise(const char* instance, const char* hostname) {
#if TESSER_MDNS
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;  // started by the application
    if (hostname && (err = mdns_hostname_set(hostname)) != ESP_OK) return err;
    if (instance) mdns_instance_name_set(instance);
    std::string api = base_.empty() ? std::string("/") : base_;
    std::string id = api_.deviceIdHex();  // the device, as remote stubs show it
    mdns_txt_item_t txt[3];
    size_t n = 0;
    txt[n++] = {"api", api.c_str()};
    if (wsUri_) txt[n++] = {"ws", wsUri_};
    if (!id.empty()) txt[n++] = {"id", id.c_str()};
    if (mdns_service_exists("_tesser", "_tcp", nullptr)) mdns_service_remove("_tesser", "_tcp");
    return mdns_service_add(instance, "_tesser", "_tcp", port_, txt, n);  // the TXT is copied
#else
    (void)instance;
    (void)hostname;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace tesser

#endif
