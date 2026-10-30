// =====================================================================
// g100_sensor.h -- G100 VFD sensor package: ISensorNode implementation.
//
// This file is where the ENVIRONMENT defines WHAT the node perceives.
// The 4 features and their order are the node's identity to the brain:
//   0: i     RMS current        [A]
//   1: slip  |f_ref - f_out|    [Hz]
//   2: vdc   DC bus voltage     [V]
//   3: tq    |torque|           [%]
// These names/units feed: UNS payloads (meas map), condition-library
// centroids, console output. Changing order invalidates old models.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "sensor_interface.h"
#include "g100_regs.h"
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
    out[0] = tele_.i_rms;
    out[1] = fabsf(tele_.slip);
    out[2] = tele_.v_dc;
    out[3] = fabsf(tele_.torque);
  }

  size_t logLine(char* out, size_t cap) override;

  const g100::Telemetry& telemetry() const { return tele_; }
  void setVerbose(bool v) { verbose_ = v; }

private:
  bool readBlock(const char* tag, uint16_t docAddr, uint8_t qty, uint16_t* out);
  void decode();

  // C++17 inline constexpr: node's feature identity lives in the header
  static constexpr FeatureInfo FEATS[4] = {
    {"i", "A"}, {"slip", "Hz"}, {"vdc", "V"}, {"tq", "%"},
  };
  static constexpr FeatureProfile PROF = {"vfd_g100", 4, FEATS};

  G100Modbus mb_;
  g100::Telemetry tele_;
  uint16_t blkA0_[g100::QTY_A0], blkA1_[g100::QTY_A1];
  uint16_t blkA2_[g100::QTY_A2], blkB_[g100::QTY_B], blkC_[1];
  uint32_t last_poll_ok_ = 0, poll_fails_ = 0;
  bool     verbose_ = false;
};
