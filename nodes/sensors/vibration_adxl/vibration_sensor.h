// =====================================================================
// vibration_sensor.h -- vibration sensor package: ISensorNode impl.
//
// WHAT the node perceives (feature identity - order is load-bearing,
// same contract as the G100 package). Locked 2026-11-22 from the CLEAN
// rig capture (tools/feature_selection.py on data/raw/cap_20261122_160441,
// after the sample-beat fix; the 2026-11-21 lock ranked features from
// the corrupted capture and is retired). On the clean capture this set
// scores 0.9907 LOO vs 0.9658 for {rms_y, entropy, margin, rms_x}, whose
// errors concentrated on speed_2_imbalance <-> speed_2_phase_loss:
//   0: rms       vibration RMS of the vector magnitude, DC-blocked  [g]
//   1: b2        mean band power over 9-24 Hz (bins 6-15 of the    [g^2]
//                64-pt spectrum) - the mechanical band
//   2: rms_y     y-axis vibration RMS, DC-blocked                  [g]
//   3: centroid  power-weighted mean spectral frequency            [Hz]
// All four come from the FFT the window already runs: rms/rms_y are
// window sums over wdev_/wvib_, b2/centroid are bin sums over binp_.
// No entropy/margin story survives the clean data (entropy ranked #7,
// F=14.75); the study's top-4 subset line is the provenance.
//
// DSP: ADXL345 sampled at 200 Hz, per-axis DC blocker at 200 Hz
// (fc ~ 0.5 Hz), decimated 2:1 (2-tap boxcar) to 100 Hz effective,
// 64-point FFT of the three axes (0.64 s window, Hann, 1.5625 Hz bins,
// DC + Nyquist excluded = 31 bins). fs=100 keeps every frequency the
// rig actually uses (the old b4 band never saw the 100 Hz line: the
// Nyquist bin is excluded) at half the FFT work of the deployed 128.
// Values are RAW, deliberately not log-scaled: wide dynamic range is
// the core's job (frozen log-range 0..1 space for centroids, Welford
// z-scores for the net).
//
// Timing: the app calls poll() every ~2-3 ms; an internal micros() beat
// keeps the actual sample rate at exactly 200 Hz regardless of loop
// jitter. One window completes every 0.64 s; the 1 Hz core step always
// finds a fresh one. Console 'R' streams raw x,y,z at 200 Hz for the
// offline feature study (tools/feature_selection.py --capture).
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

  // console 'R': one x,y,z line (g) per 200 Hz sample, for offline
  // feature studies (tools/feature_selection.py --capture)
  void toggleRawStream() override;

  const FeatureProfile& profile() const override { return PROF; }

  void features(float* out) const override;

  size_t logLine(char* out, size_t cap) override;

private:
  void processWindow();

  // C++17 inline constexpr: node's feature identity lives in the header
  static constexpr FeatureInfo FEATS[4] = {
    {"rms", "g"}, {"b2", "g2"}, {"rms_y", "g"}, {"centroid", "Hz"},
  };
  static constexpr FeatureProfile PROF = {"vib_adxl", 4, FEATS};

  static constexpr uint32_t POLL_CALL_MS = 2;  // app calls poll() at least this often
  static constexpr uint32_t TICK_US      = 5000;  // 200 Hz sample beat
  static constexpr int      FFT_N        = 64;    // 64 samples @ 100 Hz = 0.64 s
  static constexpr float    FS           = 200.0f;  // sampling beat
  static constexpr float    FS_EFF       = 100.0f;  // after the 2:1 decimation
  static constexpr float    HP_ALPHA     = 0.016f;  // DC tracker, fc ~ 0.5 Hz @ 200 Hz
  static constexpr int      FAIL_RECOVER = 50;      // consecutive bad polls -> bus recovery

  // sampler state (single instance per node, like the MVP)
  uint32_t next_us_ = 0;
  bool     have_hp_ = false;
  float    dev_x_ = 0, dev_y_ = 0, dev_z_ = 0;
  float    pair_[3] = {0, 0, 0};   // 2:1 boxcar accumulator (200 -> 100 Hz)
  bool     pair_hi_ = false;       // parity: emit on the second 200 Hz tick
  int      w_idx_ = 0;
  int      consec_fails_ = 0;
  bool     raw_ = false;           // raw x,y,z stream ('R' toggles)

  uint32_t last_win_ms_ = 0, poll_fails_ = 0;
  float    feat_[4] = {0};

  // window + FFT workspace (~5.5 KB in internal RAM, was ~11 KB)
  float    wdev_[3][FFT_N];          // DC-blocked, decimated per-axis windows
  float    wvib_[FFT_N];             // per-sample vector magnitude
  float    spec_re_[FFT_N], spec_im_[FFT_N];
  float    hann_[FFT_N];
  float    tw_r_[FFT_N / 2], tw_i_[FFT_N / 2];
  float    binp_[FFT_N / 2];         // summed power spectrum (3 axes)
  uint8_t  bitrev_[FFT_N];
};
