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

    // Installs the UART driver if nobody did (with `rxBuffer` bytes of
    // receive buffer and an event queue), then starts the reader task.
    // Handlers run in that task, under the API lock. With the driver
    // installed here, bytes lost on the way in (the hardware FIFO overflowing
    // while flash writes stall the UART interrupt, or the buffer full) are
    // noticed: the line is answered busy (line_overrun) rather than parsed,
    // and overruns() counts them. A driver installed elsewhere gives no events.
    esp_err_t begin(uint32_t stackSize = 6144, UBaseType_t priority = 5, int rxBuffer = 2048);
    void end();

    LineTransport& lines() { return line_; }
    // UART overflows (FIFO or buffer) and frame or parity errors seen.
    uint32_t overruns() const { return overruns_; }

private:
    static void taskEntry(void* arg);
    void drain();

    uart_port_t port_;
    LineTransport line_;
    TaskHandle_t task_ = nullptr;
    QueueHandle_t events_ = nullptr;
    volatile uint32_t overruns_ = 0;
    volatile bool running_ = false;
};

}  // namespace tesser

#endif
