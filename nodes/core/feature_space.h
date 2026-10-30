// =====================================================================
// feature_space.h -- frozen log-range normalizer ("condition space").
//
// Two normalizers, two purposes (do not merge them):
//   1. THIS one: log-mapped 0..1 space, ranges FROZEN at the end of CAL.
//      Used ONLY for condition-library centroids and episode summaries,
//      so a label learned today still matches six weeks from now.
//      Semantics preserved verbatim from vfd_streamq MVP (tNorm/condArm).
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
    }
    armed_ = true;
  }

  // Clamp to [lo,hi] then log-map to 0..1 (MVP tNorm, verbatim semantics).
  float to01(int f, float x) const {
    const Range& m = r_[f];
    if (x < m.lo) x = m.lo;
    if (x > m.hi) x = m.hi;
    return logf(x / m.lo) / logf(m.hi / m.lo);
  }

  float lo(int f) const { return r_[f].lo; }
  float hi(int f) const { return r_[f].hi; }

  // persistence restore (bypasses calibration)
  void setRange(int f, float lo, float hi) { r_[f].lo = lo; r_[f].hi = hi; }
  void forceArmed(bool a, uint32_t steps)  { armed_ = a; n_ = steps; }

private:
  struct Range { float mu = 0, m2 = 0, lo = cfg::SCORE_FLOOR, hi = cfg::SCORE_FLOOR * 4; };
  Range    r_[cfg::MAX_FEATURES];
  int      nf_ = 0;
  uint32_t n_ = 0;
  bool     armed_ = false;
};
