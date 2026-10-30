// =====================================================================
// g100_sensor.cpp -- register reads + decode (bench-proven, verbatim
// scaling factors from the MVP).
// =====================================================================
#include "g100_sensor.h"
#include "node_config.h"
#include <math.h>

bool G100Sensor::begin() {
  mb_.begin();
  delay(300);
  Serial.printf("[G100] RS485: %lu 8N1 slave %u, wire=doc-%u | "
                "CM.01=1 CM.02=0 CM.03=3 CM.04=0\n",
                (unsigned long)g100::BAUD, g100::SLAVE_ID, g100::LS_OFF);
  Serial.println("[G100] Only ONE RS485 master - disconnect DriveView9 while polling.");
  return true;
}

bool G100Sensor::readBlock(const char* tag, uint16_t docAddr, uint8_t qty,
                           uint16_t* out) {
  size_t n = mb_.mkRead(mb_.reqBuf(), g100::SLAVE_ID, docAddr - g100::LS_OFF, qty);
  return mb_.exchange(tag, mb_.reqBuf(), n, 5 + 2 * qty, 0x03, out, qty, verbose_);
}

void G100Sensor::decode() {
  tele_.f_ref  = blkA0_[0] * 0.01f;
  tele_.i_rms  = blkA0_[4] * 0.1f;
  tele_.f_out  = blkA0_[5] * 0.01f;
  tele_.v_out  = blkA0_[6];
  tele_.v_dc   = blkA0_[7];
  tele_.p_kw   = blkA1_[0] * 0.1f;
  tele_.status = blkA1_[1];
  tele_.fault  = blkA1_[2];
  tele_.v0pct  = blkA1_[6];
  tele_.rpm    = (int16_t)blkA2_[0];
  tele_.torque = (int16_t)blkB_[7] * 0.1f;
  tele_.tempC  = (int16_t)blkC_[0];
  tele_.slip   = tele_.f_ref - tele_.f_out;
  tele_.run      = tele_.status & (1 << 1 | 1 << 2);
  tele_.accel    = tele_.status & (1 << 4);
  tele_.decel    = tele_.status & (1 << 5);
  tele_.at_speed = tele_.status & (1 << 6);
  tele_.trip     = tele_.status & (1 << 3);
}

void G100Sensor::poll() {
  bool ok = true;
  ok &= readBlock("blkA0", g100::DOC_BLK_A0, g100::QTY_A0, blkA0_);
  ok &= readBlock("blkA1", g100::DOC_BLK_A1, g100::QTY_A1, blkA1_);
  ok &= readBlock("rpm",   g100::DOC_BLK_A2, g100::QTY_A2, blkA2_);
  ok &= readBlock("blkB",  g100::DOC_BLK_B,  g100::QTY_B,  blkB_);
  ok &= readBlock("temp",  g100::DOC_TEMP,   1,            blkC_);
  if (!ok) { poll_fails_++; return; }
  last_poll_ok_ = millis();
  decode();
}

bool G100Sensor::fresh() const {
  return last_poll_ok_ != 0 &&
         (millis() - last_poll_ok_) < cfg::FRESH_WINDOW_MS;
}

size_t G100Sensor::logLine(char* out, size_t cap) {
  return snprintf(out, cap,
    "[G100] fref=%6.2f fout=%6.2f | I=%6.2f A | Vdc=%4.0f | Tq=%6.1f%% | "
    "rpm=%5d T=%3d C | fl=0x%04X",
    tele_.f_ref, tele_.f_out, tele_.i_rms, tele_.v_dc,
    tele_.torque, tele_.rpm, tele_.tempC, tele_.fault);
}
