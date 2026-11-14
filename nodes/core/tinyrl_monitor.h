// =====================================================================
// tinyrl_monitor.h -- TinyRL-powered condition monitor (GENERAL core).
//
// Replaces the MVP's hand-rolled transition-histogram NLL with the real
// TinyRL stack:
//
//   network     nn::Sequential  Linear(nf,H,sparse) -> LayerNorm ->
//               LeakyReLU -> Linear(H,H,sparse) -> LayerNorm ->
//               LeakyReLU -> Linear(H,nf)
//   task        one-step prediction: net(x_{t-1}) ~= x_t (self-supervised,
//               no actions -> ObGD used directly, StreamQ-style loop)
//   optimizer   StreamingOptimizer (Stream-X v3 App. A): per-coordinate
//               leaky running max bounds every parameter's move by the
//               step size, regardless of network size. v2 ObGD stays
//               available behind -DMONITOR_USE_OBGD for ablation.
//   sparse init layer-aware budget (v3 App. C): global nonzero budget is
//               distributed over layers with a connectivity floor of one
//               nonzero input per unit -- the v2 uniform rule zeroed ALL
//               first-layer weights at fan_in=4 / 90% (node boots deaf).
//   obs stats   SampleMeanStd (data_normalizer.h) = the Welford engine
//               behind NormalizeObservation; used directly because its
//               stats must be frozen at theta-fit end (stationary score
//               space) and serialized for exact post-boot resume -
//               NormalizeObservation keeps them private.
//   reward      r_t = -mean squared prediction error  (maximize -surprise)
//   score       EMA of the per-step surprise; under normal conditions the
//               trained predictor makes the score stationary, any regime
//               change lifts it. theta = mu + 4 sigma (fit window).
//
// StreamQ/StreamAC wrappers are NOT used here because a condition
// monitor has no action space; the update loop below mirrors
// examples/stream_x/src/stream_q.h update() step-for-step so a future
// control node (setpoints, trips) can drop StreamQ in beside it.
// =====================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <memory>
#include <matrix.h>
#include <autograd.h>
#include <module.h>
#include <layers.h>
#if defined(MONITOR_USE_OBGD)
#include "obgd.h"                    // ablation path (v2 algorithm)
using MonitorOpt = ObGD;
#else
#include "streaming_optimizer.h"     // default (v3 StreamingOptimizer)
using MonitorOpt = StreamingOptimizer;
#endif
#include "data_normalizer.h"
#include "feature_space.h"
#include "sensor_interface.h"
#include "node_config.h"

class ConditionLibrary;

class TinyRLConditionMonitor {
public:
  struct StepOut {
    bool alarm_open;    // ALARM just raised  -> app opens an episode
    bool alarm_clear;   // ALARM just cleared -> app closes episode
  };

  void begin(const FeatureProfile& prof);
  void step(const float* raw, StepOut& out);   // 1 Hz, only with fresh data

  // user/agent gated
  void startLearning();        // fresh CAL -> FIT -> monitoring
  void factoryReset();
  void onWorldChange();        // regime changed (label event): drop traces
  void onFeedbackNormal();     // false alarm -> desensitise (raise theta)
  void onFeedbackFault();      // confirmed  -> sharpen (lower theta)
  void attachLibrary(const ConditionLibrary* lib);  // optional observer

  // state queries
  bool     learning()   const { return learning_; }
  bool     thetaReady() const { return theta_ready_; }
  bool     alarmed()    const { return alarmed_; }
  float    score()      const { return score_; }
  float    theta()      const { return theta_; }
  uint32_t steps()      const { return step_; }
  size_t   paramCount() const { return n_params_; }

  // semantic state strings for State/condition (MVP contract)
  const char* stateString() const;   // idle|learn|ok|alarm
  float confidence() const;          // 0..1

  FeatureSpace& space() { return fs_; }
  const FeatureSpace& space() const { return fs_; }

  // ---- persistence surface (used by persistence.cpp) ----
  nn::Sequential&       net()       { return net_; }
  const nn::Sequential& net() const { return net_; }

  struct State {                    // POD-friendly snapshot of scalar state
    float    score, fit_mu, fit_m2, theta;
    uint32_t step;
    uint8_t  cal, theta_ready, alarm;
    int16_t  hi, lo;
    float    fs_mu[cfg::MAX_FEATURES];              // C-space anchors (v5)
    float    fs_unit[cfg::MAX_FEATURES];            // per-feature match units (v5)
    float    fs_lo[cfg::MAX_FEATURES], fs_hi[cfg::MAX_FEATURES];
    uint8_t  fs_armed;
    uint32_t fs_steps;
    float    nm_mean[cfg::MAX_FEATURES], nm_var[cfg::MAX_FEATURES];
  };
  void exportState(State& s) const;
  bool importState(const State& s);   // false = profile size mismatch

private:
  void buildNet();                   // rebuild net + optimizer TOGETHER
  ag::Matrix frozenNorm(const ag::Matrix& x) const;

  const FeatureProfile* prof_ = nullptr;
  int  nf_ = 0;

  // TinyRL stack
  nn::Sequential        net_;
  MonitorOpt            opt_{};
  std::unique_ptr<SampleMeanStd> obsnorm_;  // Welford stats while fitting
  ag::Matrix            prev_xn_;             // last z-scored observation
  bool                  has_prev_ = false;
  size_t                n_params_ = 0;

  // frozen normalizer (after theta fit) for exact post-boot behaviour
  ag::Matrix nm_mean_, nm_var_;

  // condition space
  FeatureSpace fs_;

  // optional library observer (B-rule, 2026-11-14): features matching a
  // NORMAL-kind centroid are a known situation, not an unknown one - they
  // count as quiet regardless of surprise score. Theta stays untouched.
  const ConditionLibrary* lib_ = nullptr;

  // score pipeline + phase machine
  float    score_ = 0, last_surprise_ = 1.0f;
  float    fit_mu_ = 0, fit_m2_ = 0, theta_ = 0;
  uint32_t step_ = 0, fit_n_ = 0;
  int16_t  hi_ = 0, lo_ = 0;
  bool     learning_ = false, theta_ready_ = false, alarmed_ = false;
  float    last_raw_[cfg::MAX_FEATURES];
};
