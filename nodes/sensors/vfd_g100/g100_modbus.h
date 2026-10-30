// =====================================================================
// g100_modbus.h -- minimal Modbus-RTU master over half-duplex RS485.
// Bench-proven transport from the MVP, kept byte-for-byte (only wrapped
// in a class). Do not "optimize": the echo/gap/T35 handling encodes real
// bench behavior of the G100 + MAX485 chain.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include "g100_regs.h"

class G100Modbus {
public:
  struct Reply {
    uint8_t buf[264];
    size_t  len;
    bool    echo;
    bool    crcOk;
  };

  void begin() {
    pinMode(g100::PIN_DE, OUTPUT);
    digitalWrite(g100::PIN_DE, LOW);
    rs_.begin(g100::BAUD, SERIAL_8N1, g100::PIN_RX, g100::PIN_TX);
  }

  // Build a read-Holding-Registers frame (returns frame length).
  size_t mkRead(uint8_t* f, uint8_t id, uint16_t wireAddr, uint16_t qty) {
    f[0] = id; f[1] = 0x03;
    f[2] = wireAddr >> 8; f[3] = wireAddr & 0xFF;
    f[4] = qty >> 8;      f[5] = qty & 0xFF;
    return finish(f, 6);
  }

  // Send request (if any), listen for reply, validate CRC/id/fc/len.
  Reply transact(const uint8_t* req, size_t len);
  bool exchange(const char* tag, const uint8_t* req, size_t len,
                size_t expectLen, uint8_t fc, uint16_t* regs,
                uint16_t maxRegs, bool verbose = false);

  uint8_t* reqBuf() { return req_; }

private:
  static uint16_t crc16(const uint8_t* b, size_t n);
  static size_t   finish(uint8_t* f, size_t n);
  static uint32_t byteTimeUs(uint32_t baud);
  static uint32_t t35Us();
  static void     dumpHex(const uint8_t* d, size_t n);
  Reply listen();

  HardwareSerial& rs_ = Serial1;
  uint8_t req_[260];
};
