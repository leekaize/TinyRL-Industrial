// =====================================================================
// adxl345.cpp -- ADXL345 low-level access (bench-proven MVP logic).
// =====================================================================
#include "adxl345.h"
#include <Arduino.h>
#include <math.h>

#ifndef USE_SPI
#define USE_SPI 1
#endif

#if USE_SPI
#include <SPI.h>
#else
#include <Wire.h>
#endif

namespace adxl345 {

// ---------------- ADXL345 registers ----------------
constexpr uint8_t REG_DEVID   = 0x00;  // expects 0xE5
constexpr uint8_t REG_BW_RATE = 0x2C;
constexpr uint8_t REG_PWR_CTL = 0x2D;
constexpr uint8_t REG_INT_EN  = 0x2E;
constexpr uint8_t REG_FORMAT  = 0x31;
constexpr uint8_t REG_DATAX0  = 0x32;

// Frame decoder + physical sanity check: with +-16 g configured and gravity
// always present, impossible values mean corrupted bytes -> reject.
static bool decode6(const uint8_t* b, float& x, float& y, float& z) {
  int16_t rx = (int16_t)(b[0] | (b[1] << 8));
  int16_t ry = (int16_t)(b[2] | (b[3] << 8));
  int16_t rz = (int16_t)(b[4] | (b[5] << 8));
  x = rx / 256.0f; y = ry / 256.0f; z = rz / 256.0f;  // FULL_RES: 3.9 mg/LSB
  constexpr float AXIS_CLIP = 17.0f;                  // beyond = corrupted frame
  if (fabsf(x) > AXIS_CLIP || fabsf(y) > AXIS_CLIP || fabsf(z) > AXIS_CLIP) return false;
  if (x * x + y * y + z * z < 0.0025f) return false;  // all-zero frame (|a| < 0.05 g)
  return true;
}

#if USE_SPI
// Portable across ESP32 / ESP32-S3: the default constructor selects a valid
// SPI peripheral on any target (VSPI/HSPI names don't exist on the S3).
// Pin routing is fully controlled by the explicit begin() call below.
constexpr uint8_t  PIN_CS   = 5;
constexpr uint8_t  PIN_SCK  = 18;
constexpr uint8_t  PIN_MISO = 21;   // S3: 19/20 are USB D-/D+
constexpr uint8_t  PIN_MOSI = 17;   // S3: GPIO23 does not exist
constexpr uint32_t SPI_HZ   = 1000000;

static SPIClass adxlSPI;

static void w(uint8_t reg, uint8_t val) {
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(reg);
  adxlSPI.transfer(val);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
}

static uint8_t r1(uint8_t reg) {
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(0x80 | reg);
  uint8_t v = adxlSPI.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
  return v;
}

bool read3(float& x, float& y, float& z) {
  uint8_t b[6];
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(0xC0 | REG_DATAX0);       // read + multibyte
  for (int i = 0; i < 6; i++) b[i] = adxlSPI.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
  return decode6(b, x, y, z);
}

#else  // I2C

constexpr uint8_t  ADXL_ADDR = 0x53;   // 0x1D if SDO high
constexpr uint32_t I2C_HZ    = 100000;

static bool w(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool read3(float& x, float& y, float& z) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  return decode6(b, x, y, z);
}
#endif

bool init() {
#if USE_SPI
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (r1(REG_DEVID) != 0xE5) return false;
#else
  Wire.begin();
  Wire.setClock(I2C_HZ);
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(REG_DEVID);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL_ADDR, (uint8_t)1) != 1) return false;
  if (Wire.read() != 0xE5) return false;
#endif
  w(REG_PWR_CTL, 0x00);  // standby while configuring
  w(REG_BW_RATE, 0x0C);  // 400 Hz ODR: reading at 200 Hz always gets a fresh frame
  w(REG_FORMAT,  0x0B);  // FULL_RES, +-16 g
  w(REG_INT_EN,  0x00);  // no interrupts
  w(REG_PWR_CTL, 0x08);  // measure
  return true;
}

void recover() {
  Serial.println("[BUS] recovery: re-init transport + reconfigure ADXL345");
#if USE_SPI
  init();
#else
  Wire.end();
  delay(5);
  if (!init()) Serial.println("[BUS] recovery failed, sensor not responding");
#endif
}

} // namespace adxl345
