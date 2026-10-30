// =====================================================================
// console.h -- serial CLI of the GENERAL core (CR / LF / CRLF lines).
//   s = start learning        (machine at NORMAL load!)
//   r = poll sensor once, verbose
//   l = list condition library
//   f <label> [normal|fault]  = answer the open episode
//   L = FACTORY RESET (model + net + library)
//   m = model / runtime status
// =====================================================================
#pragma once
#include <stdint.h>

class SensorNodeApp;    // composition root (node_runtime.h)

class Console {
public:
  void begin(SensorNodeApp* app) { app_ = app; }
  void tick();                    // non-blocking line reader

private:
  void execCmd(char* s);

  SensorNodeApp* app_ = nullptr;
  char     line_[64];
  uint8_t  idx_ = 0;
};
