#include "tesser/transports/uart_transport.h"

#if defined(ESP_PLATFORM) && !defined(ARDUINO)

namespace tesser {

UartTransport::UartTransport(Api& api, uart_port_t port)
    : port_(port), line_(api, [port](const char* data, size_t len) { uart_write_bytes(port, data, len); }) {}

UartTransport::~UartTransport() { end(); }

esp_err_t UartTransport::begin(uint32_t stackSize, UBaseType_t priority) {
    if (task_) return ESP_ERR_INVALID_STATE;
    if (!uart_is_driver_installed(port_)) {
        esp_err_t err = uart_driver_install(port_, 2048, 0, 0, nullptr, 0);
        if (err != ESP_OK) return err;
    }
    running_ = true;
    if (xTaskCreate(&UartTransport::taskEntry, "tesser_uart", stackSize, this, priority, &task_) != pdPASS) {
        running_ = false;
        task_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void UartTransport::end() {
    if (!task_) return;
    running_ = false;
    // The task notices within one read timeout and deletes itself.
    while (task_) vTaskDelay(pdMS_TO_TICKS(10));
}

void UartTransport::taskEntry(void* arg) {
    auto* self = static_cast<UartTransport*>(arg);
    char buf[128];
    while (self->running_) {
        // Block for the first byte only, then drain what's already buffered:
        // asking for a full buffer would add the timeout to every request.
        int n = uart_read_bytes(self->port_, buf, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        self->line_.feed(buf, 1);
        size_t buffered = 0;
        while (uart_get_buffered_data_len(self->port_, &buffered) == ESP_OK && buffered > 0) {
            size_t chunk = buffered < sizeof(buf) ? buffered : sizeof(buf);
            n = uart_read_bytes(self->port_, buf, static_cast<uint32_t>(chunk), 0);
            if (n <= 0) break;
            self->line_.feed(buf, static_cast<size_t>(n));
        }
    }
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace tesser

#endif
