// =====================================================================
// g100_modbus.cpp -- transport implementation (bench-proven, verbatim).
// =====================================================================
#include "g100_modbus.h"

uint16_t G100Modbus::crc16(const uint8_t* b, size_t n) {
  uint16_t c = 0xFFFF;
  while (n--) { c ^= *b++; for (uint8_t i = 0; i < 8; i++)
    c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1); }
  return c;
}

uint32_t G100Modbus::byteTimeUs(uint32_t baud) { return 10000000UL / baud; }
uint32_t G100Modbus::t35Us() {
  return g100::BAUD > 19200 ? 1750 : byteTimeUs(g100::BAUD) * 35 / 10 + 200;
}

void G100Modbus::dumpHex(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n; i++) Serial.printf("%02X ", d[i]);
}

size_t G100Modbus::finish(uint8_t* f, size_t n) {
  uint16_t c = crc16(f, n); f[n] = c & 0xFF; f[n + 1] = c >> 8; return n + 2;
}

G100Modbus::Reply G100Modbus::listen() {
  Reply r; r.len = 0; r.echo = false; r.crcOk = false;
  uint32_t t0 = millis(), lastRx = micros();
  const uint32_t gap = t35Us();
  while (millis() - t0 < g100::TMO_MS) {
    while (rs_.available() && r.len < sizeof(r.buf)) {
      r.buf[r.len++] = rs_.read();
      lastRx = micros();
    }
    if (r.len && (micros() - lastRx) > gap) break;
  }
  if (r.len >= 4) {
    uint16_t c = crc16(r.buf, r.len - 2);
    r.crcOk = (r.buf[r.len - 2] == (c & 0xFF)) && (r.buf[r.len - 1] == (c >> 8));
  }
  return r;
}

G100Modbus::Reply G100Modbus::transact(const uint8_t* req, size_t len) {
  Reply r; r.len = 0; r.echo = false; r.crcOk = false;
  if (req) {
    while (rs_.available()) rs_.read();
    digitalWrite(g100::PIN_DE, HIGH);
    delayMicroseconds(50);
    rs_.write(req, len);
    rs_.flush();
    delayMicroseconds(2 * byteTimeUs(g100::BAUD) + 300);
    digitalWrite(g100::PIN_DE, LOW);
    delayMicroseconds(100);
    while (rs_.available()) rs_.read();
  }
  r = listen();
  if (req && r.len == len && memcmp(req, r.buf, len) == 0) r.echo = true;
  return r;
}

bool G100Modbus::exchange(const char* tag, const uint8_t* req, size_t len,
                          size_t expectLen, uint8_t fc, uint16_t* regs,
                          uint16_t maxRegs, bool verbose) {
  uint32_t t0 = millis();
  Reply r = transact(req, len);
  uint32_t dt = millis() - t0;
  if (verbose) { Serial.printf("[%s] TX ", tag); dumpHex(req, len); Serial.println(); }
  if (r.echo && fc != 0x05 && fc != 0x06) {
    Reply r2 = transact(nullptr, 0);
    if (r2.len) r = r2;
    else { if (verbose) Serial.printf("[%s] -- self-echo only, slave silent\n", tag); return false; }
  }
  if (r.len == 0) { if (verbose) Serial.printf("[%s] -- TIMEOUT (%lu ms)\n", tag, (unsigned long)g100::TMO_MS); return false; }
  if (!r.crcOk) { if (verbose) { Serial.printf("[%s] -- BAD CRC: ", tag); dumpHex(r.buf, r.len); Serial.println(); } return false; }
  if (r.buf[0] != req[0] || (r.buf[1] & 0x7F) != fc) {
    if (verbose) { Serial.printf("[%s] -- WRONG id/fc: ", tag); dumpHex(r.buf, r.len); Serial.println(); } return false;
  }
  if (r.buf[1] & 0x80) {
    if (verbose) Serial.printf("[%s] -- EXCEPTION 0x%02X, %lu ms\n", tag, r.buf[2], (unsigned long)dt);
    return false;
  }
  if (r.len != expectLen) {
    if (verbose) { Serial.printf("[%s] -- BAD len (%lu != %lu)\n", tag,
                  (unsigned long)r.len, (unsigned long)expectLen); }
    return false;
  }
  if (regs) {
    uint16_t n = r.buf[2] / 2;
    for (uint16_t i = 0; i < n && i < maxRegs; i++)
      regs[i] = ((uint16_t)r.buf[3 + 2*i] << 8) | r.buf[4 + 2*i];
  }
  if (verbose) { Serial.printf("[%s] OK %lu ms\n", tag, (unsigned long)dt); }
  return true;
}
