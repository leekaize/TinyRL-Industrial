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
//   optimizer   ObGD with eligibility traces; the overshooting bound
//               automatically shrinks steps when a large surprise hits,
//               so bursts (the anomaly itself) cannot destroy the model.
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
#include "obgd.h"
#include "data_normalizer.h"
#include "feature_space.h"
#include "sensor_interface.h"
#include "node_config.h"

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
  ObGD                  opt_{cfg::OBGD_LR, cfg::OBGD_GAMMA, cfg::OBGD_LAMBDA, cfg::OBGD_KAPPA};
  std::unique_ptr<SampleMeanStd> obsnorm_;  // Welford stats while fitting
  ag::Matrix            prev_xn_;             // last z-scored observation
  bool                  has_prev_ = false;
  size_t                n_params_ = 0;

  // frozen normalizer (after theta fit) for exact post-boot behaviour
  ag::Matrix nm_mean_, nm_var_;

  // condition space
  FeatureSpace fs_;

  // score pipeline + phase machine
  float    score_ = 0, last_surprise_ = 1.0f;
  float    fit_mu_ = 0, fit_m2_ = 0, theta_ = 0;
  uint32_t step_ = 0, fit_n_ = 0;
  int16_t  hi_ = 0, lo_ = 0;
  bool     learning_ = false, theta_ready_ = false, alarmed_ = false;
  float    last_raw_[cfg::MAX_FEATURES];
};
