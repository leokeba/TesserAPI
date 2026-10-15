#include "tesser/transports/uart_transport.h"

#if defined(ESP_PLATFORM) && !defined(ARDUINO)

namespace tesser {

UartTransport::UartTransport(Api& api, uart_port_t port)
    : port_(port), line_(api, [port](const char* data, size_t len) { uart_write_bytes(port, data, len); }) {}

UartTransport::~UartTransport() { end(); }

esp_err_t UartTransport::begin(uint32_t stackSize, UBaseType_t priority, int rxBuffer) {
    if (task_) return ESP_ERR_INVALID_STATE;
    if (!uart_is_driver_installed(port_)) {
        esp_err_t err = uart_driver_install(port_, rxBuffer, 0, 16, &events_, 0);
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

// Feeds the line what the driver holds.
void UartTransport::drain() {
    char buf[128];
    size_t buffered = 0;
    while (uart_get_buffered_data_len(port_, &buffered) == ESP_OK && buffered > 0) {
        size_t chunk = buffered < sizeof(buf) ? buffered : sizeof(buf);
        int n = uart_read_bytes(port_, buf, static_cast<uint32_t>(chunk), 0);
        if (n <= 0) break;
        line_.feed(buf, static_cast<size_t>(n));
    }
}

void UartTransport::taskEntry(void* arg) {
    auto* self = static_cast<UartTransport*>(arg);
    if (self->events_) {
        // By events: the driver says when bytes were lost.
        TickType_t heardAt = 0;
        while (self->running_) {
            uart_event_t ev;
            if (xQueueReceive(self->events_, &ev, pdMS_TO_TICKS(50)) != pdTRUE) {
                // A damaged line whose end was lost too: answered once the
                // line has been quiet a while, rather than left waiting.
                if (self->line_.damagedPending() && xTaskGetTickCount() - heardAt > pdMS_TO_TICKS(200)) {
                    self->line_.feed("\n", 1);
                }
                continue;
            }
            switch (ev.type) {
                case UART_DATA:
                    self->drain();
                    heardAt = xTaskGetTickCount();
                    break;
                case UART_FIFO_OVF:
                case UART_BUFFER_FULL:
                case UART_FRAME_ERR:
                case UART_PARITY_ERR:
                    // The line being read lost bytes: marked before what is
                    // buffered is read, so its end answers it. Not flushed:
                    // what follows is read on, its end with it. (A client
                    // sends a line at a time, so no other line is pending.)
                    self->overruns_ = self->overruns_ + 1;
                    self->line_.lineDamaged();
                    self->drain();
                    heardAt = xTaskGetTickCount();
                    break;
                default:
                    break;
            }
        }
        self->task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }
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
