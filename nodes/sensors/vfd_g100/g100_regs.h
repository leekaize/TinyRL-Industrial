// =====================================================================
// g100_regs.h -- LSIS LSLV-G100 register map (ENVIRONMENT-SPECIFIC).
// Bench-proven Modbus facts, kept verbatim from the MVP - do not
// re-derive: FC03/06/16 only, wire addr = doc addr - 1, max FC03 qty=8,
// 9600 8N1. Drive settings (G100 manual 7.2.2):
//   CM.01=1 CM.02=0 CM.03=3 CM.04=0 (CM changes need a drive power-cycle;
//   DriveView9 must stay offline - only ONE RS485 master).
// RS485 wiring: MAX485 DE+RE -> GPIO8, RO -> GPIO18, DI -> GPIO17.
// =====================================================================
#pragma once
#include <stdint.h>

namespace g100 {

constexpr uint32_t BAUD       = 9600;
constexpr uint8_t  SLAVE_ID   = 1;
constexpr uint32_t TMO_MS     = 400;
constexpr uint16_t LS_OFF     = 1;                 // wire addr = doc addr - 1

constexpr uint16_t DOC_BLK_A0 = 0x0005;  // docs 0h0005..0h000C
constexpr uint8_t  QTY_A0     = 8;
constexpr uint16_t DOC_BLK_A1 = 0x000D;  // docs 0h000D..0h0014
constexpr uint8_t  QTY_A1     = 8;
constexpr uint16_t DOC_BLK_A2 = 0x0015;  // doc  0h0015 motor rpm
constexpr uint8_t  QTY_A2     = 1;
constexpr uint16_t DOC_BLK_B  = 0x0310;  // docs 0h0310..0h0317
constexpr uint8_t  QTY_B      = 8;
constexpr uint16_t DOC_TEMP   = 0x032D;
constexpr uint32_t POLL_PERIOD_MS = 300;

constexpr int PIN_DE = 8;
constexpr int PIN_RX = 18;
constexpr int PIN_TX = 17;

// decoded telemetry, engineering units
struct Telemetry {
  float f_ref = 0, f_out = 0, slip = 0;
  float i_rms = 0;
  float v_out = 0, v_dc = 0;
  float p_kw = 0, torque = 0;
  int16_t rpm = 0, tempC = 0;
  uint16_t status = 0, fault = 0, v0pct = 0;
  bool run = false, accel = false, decel = false, at_speed = false, trip = false;
};

} // namespace g100
