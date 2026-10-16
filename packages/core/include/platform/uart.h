#pragma once

#include <cstddef>
#include <cstdint>

// Target-owned implementation. Unsupported platforms report unavailable.
extern "C" {
bool gea_uart_open(int port, int tx, int rx, int baud, int rxBytes, int txBytes)
    __attribute__((weak));
bool gea_uart_is_open(int port) __attribute__((weak));
void gea_uart_close(int port) __attribute__((weak));
int gea_uart_read(int port, void *bytes, std::size_t capacity)
    __attribute__((weak));
int gea_uart_write(int port, const void *bytes, std::size_t length)
    __attribute__((weak));
int gea_uart_available(int port) __attribute__((weak));
int gea_uart_writable(int port) __attribute__((weak));
}

namespace gea::platform::uart {
struct Config {
  int tx;
  int rx;
  int baudRate;
  int rxBufferSize = 4096;
  int txBufferSize = 4096;
};

class Uart {
public:
  static bool open(int port, const Config &config) {
    return gea_uart_open &&
           gea_uart_open(port, config.tx, config.rx, config.baudRate,
                         config.rxBufferSize, config.txBufferSize);
  }
  static bool isOpen(int port) {
    return gea_uart_is_open && gea_uart_is_open(port);
  }
  static void close(int port) {
    if (gea_uart_close)
      gea_uart_close(port);
  }
  // Nonblocking. -1 means unavailable/closed/error; 0 means no progress.
  static int read(int port, void *bytes, std::size_t capacity) {
    return gea_uart_read ? gea_uart_read(port, bytes, capacity) : -1;
  }
  // Enqueues the complete buffer, or returns 0 if it cannot fit yet.
  static int write(int port, const void *bytes, std::size_t length) {
    return gea_uart_write ? gea_uart_write(port, bytes, length) : -1;
  }
  static int available(int port) {
    return gea_uart_available ? gea_uart_available(port) : -1;
  }
  static int writable(int port) {
    return gea_uart_writable ? gea_uart_writable(port) : -1;
  }
};
} // namespace gea::platform::uart
