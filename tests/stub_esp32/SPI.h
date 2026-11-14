// Host-side SPI stub (compile verification only, NOT shipped).
#pragma once
#include <stdint.h>

#define MSBFIRST 1
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

class SPISettings {
public:
  SPISettings(uint32_t, int, int) {}
};

class SPIClass {
public:
  void begin(int = -1, int = -1, int = -1, int = -1) {}
  void beginTransaction(const SPISettings&) {}
  uint8_t transfer(uint8_t v) { return v; }
  void endTransaction() {}
};
