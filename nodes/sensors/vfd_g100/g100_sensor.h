// =====================================================================
// g100_sensor.h -- G100 VFD + PM1125H meter sensor package: ISensorNode
// implementation.
//
// This file is where the ENVIRONMENT defines WHAT the node perceives.
// The 4 features and their order are the node's identity to the brain:
//   0: fset  commanded set frequency [Hz]  (VFD register read, no motor
//                                         feedback needed - user fact,
//                                         2026-11-14)
//   1: v     meter voltage L1-N      [V]   (PM1125H on the drive input,
//                                         single-phase 1P.Ln)
//   2: i     meter current           [A]   (PM1125H replaces the VFD's
//                                         own current register - user
//                                         decision, 2026-11-20)
//   3: pf    |power factor|          [-]   (PM1125H; sign dropped for the
//                                         log-ratio condition space,
//                                         same reason as |torque|)
// The VFD contributes only the setpoint; electrical truth comes from the
// meter (2026-11-20). vdc/tq stay in telemetry, out of the identity.
// These names/units feed: UNS payloads (meas map), condition-library
// centroids, console output. Changing order invalidates old models.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "sensor_interface.h"
#include "g100_regs.h"
#include "pm1125h_regs.h"
#include "g100_modbus.h"

class G100Sensor : public ISensorNode {
public:
  bool begin() override;
  void poll() override;
  uint32_t pollPeriodMs() const override { return g100::POLL_PERIOD_MS; }

  bool     fresh() const override;
  uint32_t lastOkMs() const override { return last_poll_ok_; }
  uint32_t failCount() const override { return poll_fails_; }

  const FeatureProfile& profile() const override { return PROF; }

  void features(float* out) const override {
    out[0] = tele_.f_ref;            // commanded setpoint (VFD)
    out[1] = ptele_.v;               // meter L1-N voltage
    out[2] = ptele_.i;               // meter current
    out[3] = fabsf(ptele_.pf);       // sign dropped for the ratio space
  }

  size_t logLine(char* out, size_t cap) override;

  const g100::Telemetry& telemetry() const { return tele_; }
  void setVerbose(bool v) { verbose_ = v; }

private:
  bool readBlock(const char* tag, uint16_t docAddr, uint8_t qty, uint16_t* out);
  bool readPMFloat(const char* tag, uint16_t docAddr, float* out);
  void decode();

  // C++17 inline constexpr: node's feature identity lives in the header
  static constexpr FeatureInfo FEATS[4] = {
    {"fset", "Hz"}, {"v", "V"}, {"i", "A"}, {"pf", ""},
  };
  static constexpr FeatureProfile PROF = {"vfd_g100", 4, FEATS};

  G100Modbus mb_;
  g100::Telemetry tele_;
  pm::Telemetry   ptele_;
  uint16_t blkA0_[g100::QTY_A0], blkA1_[g100::QTY_A1];
  uint16_t blkA2_[g100::QTY_A2], blkB_[g100::QTY_B], blkC_[1];
  uint16_t pmR_[2];
  uint32_t last_poll_ok_ = 0, poll_fails_ = 0;
  bool     verbose_ = false;
};
