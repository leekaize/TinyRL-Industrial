// =====================================================================
// adxl345.h -- register-level ADXL345 driver. Register sequence and
// scaling are bench-proven in the vibration_streamq MVP (carried over
// verbatim); only the packaging changed.
//
// Transport is compile-time: -DUSE_SPI=0 selects I2C (default is SPI;
// SPI proved robust near the motor in the MVP).
//
// SPI wiring (YD-ESP32-S3): VCC->3V3 GND->GND CS->GPIO5
//   SCL->SCK(18) SDA->MOSI(17) SDO->MISO(21)   (S3: 19/20 are USB D-/D+)
// I2C: addr 0x53 (0x1D if SDO is high), 100 kHz.
// =====================================================================
#pragma once
#include <stdint.h>

namespace adxl345 {

bool init();      // configure the sensor; false = hardware absent
bool read3(float& x, float& y, float& z);  // one 3-axis frame in g
void recover();   // re-init transport + sensor after bus failure

} // namespace adxl345
