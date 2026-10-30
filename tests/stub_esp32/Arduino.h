// Host-side Arduino stub for compile/run verification only (NOT shipped).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <functional>
#include <utility>

#define SERIAL_8N1 0x800001c
#define OUTPUT 1
#define LOW 0
#define HIGH 1
#define WL_CONNECTED 3
#define WIFI_STA 1
#define WIFI_POWER_11dBm 11

// Auto-advancing host clock: every call moves time forward so timeout
// loops (Modbus T35/T1.5) terminate. NOT for tests that need real gaps.
extern unsigned long g_host_millis;

inline unsigned long millis() { g_host_millis += 13; return g_host_millis; }
inline unsigned long micros() { return millis() * 1000 + 50; }
inline void delay(unsigned long) {}
inline void delayMicroseconds(unsigned int) {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

// ---- String (subset used by the code) ---------------------------------
class String {
public:
  String() {}
  String(const char* s) : s_(s) {}
  size_t length() const { return s_.size(); }
  const char* c_str() const { return s_.c_str(); }
private:
  std::string s_;
};

// ---- HardwareSerial / Serial ------------------------------------------
struct EspStub { unsigned getFreeHeap() { return 8 * 1024 * 1024; } };
extern EspStub ESP;

class HardwareSerial {
public:
  HardwareSerial(bool silent = false) : silent_(silent) {}
  void begin(unsigned long baud, int, int = -1, int = -1) {}
  void begin(unsigned long) {}
  explicit operator bool() const { return true; }
  int available() { return (int)rx_.size(); }
  int read() {
    if (rx_.empty()) return -1;
    char c = rx_.front();
    rx_.erase(rx_.begin());
    return (int)c;
  }
  size_t write(const uint8_t* p, size_t n) {
    if (!silent_) fwrite(p, 1, n, stdout);
    return n;
  }
  size_t write(uint8_t c) { return write(&c, 1); }
  void flush() { fflush(stdout); }
  template <typename... A> size_t printf(const char* f, A... a) {
    return fprintf(stdout, f, a...);
  }
  template <typename... A> size_t println(const char* f, A... a) {
    size_t n = fprintf(stdout, f, a...);
    fprintf(stdout, "\n");
    return n + 1;
  }
  size_t println() { fprintf(stdout, "\n"); return 1; }
  void feed(const char* s) { while (*s) rx_.push_back(*s++); }
private:
  bool silent_;
  std::string rx_;
};

extern HardwareSerial Serial;
extern HardwareSerial Serial1;   // constructed silent (RS485 on target)
