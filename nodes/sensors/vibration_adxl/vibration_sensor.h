// =====================================================================
// vibration_sensor.h -- vibration sensor package: ISensorNode impl.
//
// WHAT the node perceives (feature identity - order is load-bearing,
// same contract as the G100 package):
//   0: rms   vibration RMS, DC-blocked          [g]
//   1: b1    mean band power, 1.6-7.8 Hz        [g2]
//   2: b2    mean band power, 9.4-23.4 Hz       [g2]
//   3: b3    mean band power, 25-54.7 Hz        [g2]
//   4: b4    mean band power, 56.3-98.4 Hz      [g2]
//   5: fmax  dominant FFT bin frequency         [Hz]
// Bands come from a 128-point FFT of three DC-blocked axes at 200 Hz
// (0.64 s window, Hann). Values are RAW, deliberately not log-scaled:
// wide dynamic range is the core's job (frozen log-range 0..1 space for
// centroids, Welford z-scores for the net).
//
// Timing: the app calls poll() every ~2-3 ms; an internal micros() beat
// keeps the actual sample rate at exactly 200 Hz regardless of loop
// jitter, so FFT bin frequencies stay honest. One window completes every
// 0.64 s; the 1 Hz core step always finds a fresh one.
// =====================================================================
#pragma once
#include <Arduino.h>
#include "sensor_interface.h"
#include "adxl345.h"

class VibrationSensor : public ISensorNode {
public:
  bool begin() override;
  void poll() override;
  uint32_t pollPeriodMs() const override { return POLL_CALL_MS; }

  bool     fresh() const override;
  uint32_t lastOkMs() const override { return last_win_ms_; }
  uint32_t failCount() const override { return poll_fails_; }

  const FeatureProfile& profile() const override { return PROF; }

  void features(float* out) const override;

  size_t logLine(char* out, size_t cap) override;

private:
  void processWindow();

  // C++17 inline constexpr: node's feature identity lives in the header
  static constexpr FeatureInfo FEATS[6] = {
    {"rms", "g"}, {"b1", "g2"}, {"b2", "g2"}, {"b3", "g2"}, {"b4", "g2"},
    {"fmax", "Hz"},
  };
  static constexpr FeatureProfile PROF = {"vib_adxl", 6, FEATS};

  static constexpr uint32_t POLL_CALL_MS = 2;  // app calls poll() at least this often
  static constexpr uint32_t TICK_US      = 5000;  // 200 Hz sample beat
  static constexpr int      FFT_N        = 128;   // 128 samples @ 200 Hz = 0.64 s
  static constexpr float    FS           = 200.0f;
  static constexpr float    HP_ALPHA     = 0.016f;  // DC tracker, fc ~ 0.5 Hz @ 200 Hz
  static constexpr int      FAIL_RECOVER = 50;      // consecutive bad polls -> bus recovery

  // sampler state (single instance per node, like the MVP)
  uint32_t next_us_ = 0;
  bool     have_hp_ = false;
  float    dev_x_ = 0, dev_y_ = 0, dev_z_ = 0;
  int      w_idx_ = 0;
  int      consec_fails_ = 0;

  uint32_t last_win_ms_ = 0, poll_fails_ = 0;
  float    feat_[6] = {0};

  // window + FFT workspace (~11 KB in internal RAM, as in the MVP)
  float    wdev_[3][FFT_N];          // DC-blocked per-axis windows
  float    wvib_[FFT_N];             // per-sample vector magnitude
  float    spec_re_[FFT_N], spec_im_[FFT_N];
  float    hann_[FFT_N];
  float    tw_r_[FFT_N / 2], tw_i_[FFT_N / 2];
  float    binp_[FFT_N / 2];         // summed power spectrum (3 axes)
  uint8_t  bitrev_[FFT_N];
};
