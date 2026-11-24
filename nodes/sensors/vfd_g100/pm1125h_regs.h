// =====================================================================
// pm1125h_regs.h -- Schneider PM1125H power meter register map
// (ENVIRONMENT-SPECIFIC, user facts 2026-11-20).
// Shares the G100 RS485 bus (9600 8E1, one master) as slave 2.
// Addressing rule differs from the LS drive: wire = doc - 40001.
// Values are 32-bit floats, MSRF: the first register sent carries the
// high word. Single-phase wiring (1P.Ln): phase-1 quantities.
// =====================================================================
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace pm {

constexpr uint8_t  SLAVE_ID = 2;      // user fact 2026-11-20
constexpr uint16_t OFF      = 40001;  // wire addr = doc addr - 40001

constexpr uint16_t DOC_I    = 43000;  // current        [A]
constexpr uint16_t DOC_V    = 43028;  // voltage L1-N   [V]
constexpr uint16_t DOC_P    = 43054;  // active power   [kW]
constexpr uint16_t DOC_PF   = 43192;  // power factor, signed

// Settling time before the FIRST meter transaction of a poll. The meter
// answers 4.2 ms after its OWN replies (pmP/pmPF in the 2026-11-21 rig
// trace) but deterministically drops the first frame that follows the
// slave-1 burst - pmV timed out on every poll at the T3.5 gap while its
// three neighbours answered in ~38 ms. 400 ms of idle is enough (pmI
// always follows the pmV timeout and never fails). 25 ms is the first
// guess between those bounds; raise it if pmV still times out.
constexpr uint32_t FIRST_READ_SETTLE_MS = 25;

// meter telemetry, engineering units
struct Telemetry {
  float v = 0, i = 0, kw = 0, pf = 0;
};

// Decode one float (2 registers, MSRF). false on non-finite: the meter
// sends NaN for undefined values (e.g. PF at no load) -> poll fails
// instead of feeding a NaN into the condition space.
inline bool decodeFloat(const uint16_t* regs, float* out) {
  uint32_t u = ((uint32_t)regs[0] << 16) | regs[1];
  float f;
  memcpy(&f, &u, sizeof f);
  if (!isfinite(f)) return false;
  *out = f;
  return true;
}

} // namespace pm
