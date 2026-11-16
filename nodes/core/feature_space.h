// =====================================================================
// feature_space.h -- frozen anchored-ratio condition space ("C mapping").
//
// Two normalizers, two purposes (do not merge them):
//   1. THIS one: maps each feature to log2((x+A)/(mu+A)) decades, where
//      mu = CAL mean and A = 6*CAL sigma (the ANCHOR, 2026-11-15).
//      Used ONLY for condition-library centroids, the alarm-quiet path
//      and episode summaries.
//      Why anchored: a bare log2(x/mu) breaks on the 2026-11-15
//      stopped-machine commissioning. A near-dead CAL signal (drive
//      current/torque/setpoint read ~0 while stopped, vib bands ~1e-6)
//      makes mu ~ 0 or negative: negative mu stored NaN centroids (NaN
//      compares false and the feature passed EVERY match), and the
//      SCORE_FLOOR guard collapsed the running states - every state far
//      above the floor clamped to the same rel, so speed_1, speed_2 and
//      phase_loss became one indistinguishable point (bench: the drive
//      went structurally blind once speed_1 was labelled). The anchor
//      keeps dead signals finite and ordered (x=0 maps to a finite
//      negative coordinate, not NaN) while leaving healthy features
//      nearly unchanged (A << mu there).
//   2. TinyRL NormalizeObservation (inside tinyrl_monitor): running
//      Welford z-scores that feed the network while it trains.
//
// calibrateStep() is Welford; arm() freezes mu/sd, derives the anchor
// and each feature's matching unit.
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

  // Freeze moments. Must be called exactly once, after calReady().
  // Also derives each feature's anchor (6 sigma) and matching unit
  // (decades of one calibrated sigma, measured IN the rel coordinate).
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
      m.sd = sd;
      // anchor: 6 sigma so the whole CAL noise band (uniform fixtures
      // reach +-1.7 sd) stays clear of the guarded floor
      m.anchor = fmaxf(6.0f * sd, cfg::SCORE_FLOOR);
      float denom = fabsf(m.mu) + m.anchor;
      m.unit = fmaxf(log2f(1.0f + sd / denom), 0.05f);
    }
    armed_ = true;
  }

  // The condition position: log2((x+A)/(mu+A)) decades, clamped for
  // float sanity (mu -> 0, 2*mu -> ~+1 when A is small). Dead CAL
  // signals stay finite and ordered; x = 0 maps below mu, never NaN.
  float rel(int f, float x) const {
    const Range& m = r_[f];
    float denom = fabsf(m.mu) + m.anchor;
    float num = x + m.anchor;
    float floor_num = denom * 1e-3f;              // >= -9.97 decades
    if (num < floor_num) num = floor_num;
    float d = log2f(num / denom);
    if (d > cfg::REL_CLAMP) d = cfg::REL_CLAMP;
    if (d < -cfg::REL_CLAMP) d = -cfg::REL_CLAMP;
    return d;
  }

  // Per-feature matching unit: decades of ONE calibrated sigma, scaled
  // by the anchored level (feature_space.h arm()). Fallback for
  // conditions that carry no measured units of their own yet.
  float unit(int f) const { return r_[f].unit; }
  void setUnit(int f, float u) { r_[f].unit = u; }

  float mu(int f) const { return r_[f].mu; }
  void setMu(int f, float mu) { r_[f].mu = mu; }   // persistence restore

  float lo(int f) const { return r_[f].lo; }
  float hi(int f) const { return r_[f].hi; }
  float anchor(int f) const { return r_[f].anchor; }

  // persistence restore (bypasses calibration)
  void setRange(int f, float lo, float hi) { r_[f].lo = lo; r_[f].hi = hi; }
  void setAnchor(int f, float a) { r_[f].anchor = a; }
  void forceArmed(bool a, uint32_t steps)  { armed_ = a; n_ = steps; }

private:
  struct Range {
    float mu = 0, m2 = 0, unit = 0.05f;
    float sd = 0, anchor = cfg::SCORE_FLOOR;
    float lo = cfg::SCORE_FLOOR, hi = cfg::SCORE_FLOOR * 4;
  };
  Range    r_[cfg::MAX_FEATURES];
  int      nf_ = 0;
  uint32_t n_ = 0;
  bool     armed_ = false;
};
