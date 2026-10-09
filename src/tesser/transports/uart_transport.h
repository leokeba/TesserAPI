#pragma once

// ESP-IDF only: JSON envelopes over a UART, one per line. On Arduino, use
// StreamTransport with Serial instead.
#if defined(ESP_PLATFORM) && !defined(ARDUINO)

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesser/line_transport.h"

namespace tesser {

class UartTransport {
public:
    explicit UartTransport(Api& api, uart_port_t port = UART_NUM_0);
    ~UartTransport();

    // Installs the UART driver if nobody did, then starts the reader task.
    // Handlers run in that task, under the API lock.
    esp_err_t begin(uint32_t stackSize = 6144, UBaseType_t priority = 5);
    void end();

    LineTransport& lines() { return line_; }

private:
    static void taskEntry(void* arg);

    uart_port_t port_;
    LineTransport line_;
    TaskHandle_t task_ = nullptr;
    volatile bool running_ = false;
};

}  // namespace tesser

#endif
