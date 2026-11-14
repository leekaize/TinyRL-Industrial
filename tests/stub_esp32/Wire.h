// Host-side I2C stub (compile verification only, NOT shipped).
#pragma once
#include <stdint.h>

class TwoWire {
public:
  void begin(int = -1, int = -1) {}
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 0; }
  uint8_t requestFrom(uint8_t, uint8_t) { return 0; }
  int read() { return 0; }
  void end() {}
};
