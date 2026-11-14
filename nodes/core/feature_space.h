// =====================================================================
// feature_space.h -- frozen ratio-to-normal condition space ("C mapping").
//
// Two normalizers, two purposes (do not merge them):
//   1. THIS one: maps each feature to 0..1 by RATIO to its CAL mean,
//      two-sided around 0.5, dynamic range CONDITION_K per half scale
//      (user decision 2026-11-14, replaces the MVP clamp-to-range log
//      mapping whose +-3-sigma edges saturated: every strong fault
//      landed on the same point and loose_mount matched imbalance).
//      Used ONLY for condition-library centroids, the alarm-quiet path
//      and episode summaries. Severity scales naturally: a 2x excursion
//      sits higher than a 1.5x one.
//   2. TinyRL NormalizeObservation (inside tinyrl_monitor): running
//      Welford z-scores that feed the network while it trains.
//
// calibrateStep() is Welford; arm() freezes lo/hi = mu +- 3*sigma with
// the near-constant-signal guard from the MVP.
// =====================================================================
#pragma once
#include <math.h>
#include <string.h>
#include <stdint.h>
#include "node_config.h"

class FeatureSpace {
public:
  void begin(int nf) {
    nf_ = nf;
    for (int f = 0; f < cfg::MAX_FEATURES; ++f) r_[f] = Range{};
    n_ = 0; armed_ = false;
  }

  int      nFeatures() const { return nf_; }
  bool     armed()     const { return armed_; }
  uint32_t calSteps()  const { return n_; }
  bool     calReady()  const { return n_ >= cfg::CAL_STEPS; }

  void calibrateStep(const float* x) {
    n_++;
    for (int f = 0; f < nf_; ++f) {
      Range& m = r_[f];
      float d = x[f] - m.mu;
      m.mu += d / (float)n_;
      m.m2 += d * (x[f] - m.mu);
    }
  }

  // Freeze ranges. Must be called exactly once, after calReady().
  // Also derives each feature's matching unit (decades of one sigma).
  void arm() {
    for (int f = 0; f < nf_; ++f) {
      Range& m = r_[f];
      float sd = sqrtf(m.m2 / (float)(n_ > 1 ? n_ : 1));
      float lo = m.mu - 3 * sd, hi = m.mu + 3 * sd;
      if (hi - lo < 0.02f * fabsf(m.mu)) {      // near-constant signal
        lo = 0.9f * fabsf(m.mu);
        hi = 1.1f * fabsf(m.mu) + 10 * cfg::SCORE_FLOOR;
      }
      m.lo = fmaxf(lo, cfg::SCORE_FLOOR);
      m.hi = fmaxf(hi, m.lo * 1.02f);
      float mu = m.mu;
      if (mu < cfg::SCORE_FLOOR) mu = cfg::SCORE_FLOOR;
      m.unit = fmaxf(log2f(1.0f + sd / mu), 0.05f);
    }
    armed_ = true;
  }

  // Map to log2(x/mu) decades, clamped (the condition position):
  //   mu -> 0,  2*mu -> +1,  mu/2 -> -1.  Dead CAL signal guarded.
  float rel(int f, float x) const {
    const Range& m = r_[f];
    float mu = m.mu;
    if (mu < cfg::SCORE_FLOOR) mu = cfg::SCORE_FLOOR;
    float d = log2f(x / mu);
    if (d > cfg::REL_CLAMP) d = cfg::REL_CLAMP;
    if (d < -cfg::REL_CLAMP) d = -cfg::REL_CLAMP;
    return d;
  }

  // Per-feature matching unit: decades of ONE calibrated sigma. The
  // matching radius is q * unit(f) so it scales with each feature's own
  // noise (vib bands are 20x noisier than the drive's set-frequency
  // register). Floor 0.05 dec: no feature gets a sub-3.5% radius.
  float unit(int f) const { return r_[f].unit; }
  void setUnit(int f, float u) { r_[f].unit = u; }

  float mu(int f) const { return r_[f].mu; }
  void setMu(int f, float mu) { r_[f].mu = mu; }   // persistence restore

  float lo(int f) const { return r_[f].lo; }
  float hi(int f) const { return r_[f].hi; }

  // persistence restore (bypasses calibration)
  void setRange(int f, float lo, float hi) { r_[f].lo = lo; r_[f].hi = hi; }
  void forceArmed(bool a, uint32_t steps)  { armed_ = a; n_ = steps; }

private:
  struct Range {
    float mu = 0, m2 = 0, unit = 0.05f;
    float lo = cfg::SCORE_FLOOR, hi = cfg::SCORE_FLOOR * 4;
  };
  Range    r_[cfg::MAX_FEATURES];
  int      nf_ = 0;
  uint32_t n_ = 0;
  bool     armed_ = false;
};
