// =====================================================================
// esp_shim.c -- getentropy() for TinyRL's std::random_device (rng.h).
// Required on ESP32: <random>'s std::random_device calls getentropy(),
// which older version of  newlib does not provide. Making it weak definition
// which works across different version. We back it with the ESP32-S3 hardware
// TRNG (esp_random), same shim TinyRL ships in examples/stream_x_esp32.
// =====================================================================
#include <stddef.h>
#include <stdint.h>
#include "esp_random.h"

__attribute__((weak)) int getentropy(void *buf, size_t buflen) {
  uint8_t *buffer = (uint8_t *)buf;
  for (size_t i = 0; i < buflen; i++) {
    buffer[i] = (uint8_t)(esp_random() & 0xFF);
  }
  return 0;
}
