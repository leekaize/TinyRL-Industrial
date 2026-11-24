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
    bool settled;       // false = mid-transition (gate froze decisions)
    int  matched = -1;  // library index matched this step (-1 = unknown)
    float gate = 0;     // threshold ACTIVE this step: per-condition band
                        // (band eras) or global theta; -1 = quiet by name
    float ragg = 0;     // cond era: positional aggregate sum((rel-c)/u)^2
    float gate_r = -1;  // against the matched condition's own band (-1 =
                        // immature/inactive). Static-offset evidence; the
                        // one-step predictor is blind to it (identity).
    int  pos_ref = -1;  // WHICH condition's band gate_r belongs to (the
                        // matched condition, or the nearest one while
                        // unknown; -1 = no gate). The figure marks the
                        // switches so a ratio jump is never misread
                        // 0/-1 in the z and rel eras.
    float acc = 0;      // band eras: evidence-accumulator level (see
                        // node_config.h); 0 = fully quiet evidence

    // decision-trace extension (2026-11-21, console 'd'): every quantity
    // that drove this step's decisions. Serial console only - the MQTT
    // schema is untouched.
    float surprise = 0;   // raw one-step prediction error BEFORE the EMA
    float ev = 0;         // evidence this step added (band eras; 0 in grace)
    bool  in_grace = false; // matched but the score band is still forming
    bool  sticky = false;   // match held by the radius-edge stickiness
    bool  trained = false;  // the optimizer updated the weights this step
    float lr_frac = -1.0f;  // rate fraction used (-1 = pre-theta, fixed lr)
    float d_match = -1;     // worst-feature normalized distance to matched
    int   frame = -1;       // condition whose frame the obs was centered on
  };

  void begin(const FeatureProfile& prof);
  void step(const float* raw, StepOut& out);   // 1 Hz, only with fresh data

  // user/agent gated
  void startLearning();        // fresh CAL -> FIT -> monitoring
  void factoryReset();
  void onWorldChange();        // regime changed (label event): drop traces
  void onFeedbackNormal();     // false alarm -> desensitise (raise theta)
  void onFeedbackFault();      // confirmed  -> sharpen (lower theta)
  void voidPosEvidence();      // the operator named the state: the accumulated
                               // positional case is closed (a stale pos_acc_
                               // reservoir must not re-fire borderline moments
                               // post-label, run-120222 flaps 206..228)
  void attachLibrary(ConditionLibrary* lib);        // observer (reads +

  // diagnostics: prefix every [WHY]/[RL] print with this node's id so two
  // interleaved monitors stay attributable in one log (2026-11-22g churn
  // trace). Empty default = unchanged output on the MCU.
  void setNodeTag(const char* tag) { node_tag_ = tag; }

  // state queries
  bool     learning()   const { return learning_; }
  bool     thetaReady() const { return theta_ready_; }
  bool     alarmed()    const { return alarmed_; }
  bool     settled()    const { return settled_; }
  float    score()      const { return score_; }
  float    theta()      const { return theta_; }
  float    noveltyBase() const { return sr_base_; }  // slow surprise baseline
  int      zHi()        const { return hi_; }       // z-era Schmitt counters
  int      zLo()        const { return lo_; }
  uint32_t steps()      const { return step_; }
  size_t   paramCount() const { return n_params_; }

  // semantic state strings for State/condition (MVP contract)
  const char* stateString() const;   // idle|learn|ok|alarm
  float confidence() const;          // 0..1

  FeatureSpace& space() { return fs_; }
  const FeatureSpace& space() const { return fs_; }
  const char* nodeTag() const { return node_tag_; }

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
    float    fs_anchor[cfg::MAX_FEATURES];          // rel-space anchors (v6)
    float    fs_lo[cfg::MAX_FEATURES], fs_hi[cfg::MAX_FEATURES];
    uint8_t  fs_armed;
    uint32_t fs_steps;
    float    nm_mean[cfg::MAX_FEATURES], nm_var[cfg::MAX_FEATURES];
  };
  void exportState(State& s) const;
  bool importState(const State& s);   // false = profile size mismatch

private:
  void buildNet();                   // rebuild net + optimizer TOGETHER
  void resetSettle();                // motion EMAs + calm counter
  ag::Matrix frozenNorm(const ag::Matrix& x) const;

  const FeatureProfile* prof_ = nullptr;
  int  nf_ = 0;
  const char* node_tag_ = "";      // diagnostics prefix, see setNodeTag()

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
  // 2026-11-19: non-const - the monitor also feeds each condition's own
  // score band (per-condition calibration, condition_library.h).
  ConditionLibrary* lib_ = nullptr;

  // score pipeline + phase machine
  float    score_ = 0, last_surprise_ = 1.0f;
  float    fit_mu_ = 0, fit_m2_ = 0, theta_ = 0;
  uint32_t step_ = 0, fit_n_ = 0;
  int16_t  hi_ = 0, lo_ = 0;                        // z-era Schmitt counters
  float    alarm_acc_ = 0;                          // band-era evidence level
  float    pos_acc_ = 0;                            // cond era: POSITIONAL evidence
                                                    // only (ragg/gate_r - 1). Fires
                                                    // after the breach STANDS for
                                                    // POS_FIRE_STAND_N settled steps
                                                    // (a reservoir that outlived its
                                                    // cause flap-alarmed 11x post-label,
                                                    // run-112814 206..228; the one-step
                                                    // standing rule alarm-edged every
                                                    // churn visit once the net-drift
                                                    // settle gate settled through the
                                                    // blocks' warm-up rows, 2026-11-22)
  int      pos_breach_n_ = 0;                       // consecutive settled steps with
                                                    // the positional breach standing
                                                    // (the pos fire's standing proof)
  int      score_breach_n_ = 0;                     // consecutive settled non-grace
                                                    // steps with score over the active
                                                    // gate (the score fire's standing
                                                    // proof, SCORE_FIRE_STAND_N)
  int      quiet_n_ = 0;                            // consecutive settled steps with NO
                                                    // breach on either arm - the m-of-N
                                                    // Schmitt's clear counter (the
                                                    // ABL_ACCUMULATOR=0 fallback only)
  bool     learning_ = false, theta_ready_ = false, alarmed_ = false;
  float    last_raw_[cfg::MAX_FEATURES];

  // settle gate state (volatile by design: recomputed live, ~warmup
  // after boot/resume, once)
  float    rel_hist_[cfg::SETTLE_WINDOW_N][cfg::MAX_FEATURES];
  int      rel_hist_n_ = 0;                   // ring rows filled (caps at WINDOW_N)
  int      rel_hist_i_ = 0;                   // next write slot
  bool     settled_ = true, has_prev_rel_ = false;
  bool     quiet_latch_ = false;              // B-rule schmitt state (step())
  bool     prev_settled_ = true;              // rising edge = regime entry
  int      cond_frame_ = -2;                  // cond era: last matched frame
                                              // (frame-switch optimizer reset)
  int      sticky_km_ = -1;                   // band eras: last matched
                                              // condition (radius-edge stickiness;
                                              // RETAINED across unmatched steps)
  int      km_prev_ = -1;                     // band eras: last step's km, so the
                                              // left/matched [WHY] lines fire once
  bool     prev_sticky_ = false;              // last step's sticky state
  bool     prev_grace_ = false;               // last step's grace state
  int      reentry_grace_ = 0;                // all eras: steps left of the
                                              // re-entry grace (regime entry
                                              // to a MATURED known condition:
                                              // the band gate is not
                                              // representative while the net
                                              // re-adapts, node_config.h)
  int      floor_excess_n_ = 0;               // cond era: consecutive settled
                                              // matched steps above the state's
                                              // own matured gate (sustained
                                              // floor drift, node_config.h
                                              // FLOOR_EXCESS_N)
  int      floor_excess_km_ = -1;             // condition the counter belongs to
  float    prev_ragg_ = 0.0f;                 // cond era: last step's r_agg (same
                                              // condition only) - the geometry-
                                              // jump guard, node_config.h
  int      prev_ragg_km_ = -1;
  int      pos_ex_n_ = 0;                     // cond era: settled matched breach
                                              // steps of the current positional
                                              // excursion (< 0 = disarmed by a
                                              // fault-like jump, node_config.h
                                              // POS_EX_MIN_N)
  float    pos_ex_max_ = 0.0f;                // the excursion's peak r_agg: the
                                              // value reseedPosBand anchors at
  int      pos_sub_n_ = 0;                    // consecutive settled matched steps
                                              // back INSIDE the gate (a full
                                              // POS_EX_SUBSIDE_N proves the
                                              // excursion was the state's own
                                              // wander, not a fault)
  int      sticky_hold_n_ = 0;                // cond era: consecutive settled held
                                              // steps without a genuine match
                                              // (sticky-hold tenure release,
                                              // node_config.h STICKY_RELEASE_N)
  int      unknown_run_ = 0;                  // cond era: consecutive settled UNKNOWN
                                              // steps. The 1-second ramp row sits
                                              // between two states and settles (its
                                              // rel motion is sub-threshold) - one
                                              // settled unknown step charged 103x the
                                              // nearest band and alarm-falsied mid-tour
                                              // (run-120222 @604). Evidence charges only
                                              // from the 2nd: a transient never persists,
                                              // a fault's unknown does
  bool     hold_burned_ = false;              // cond era: the hold released on
                                              // sustained excess; no re-hold
                                              // until a genuine match
  float    sr_base_ = 1.0f;                   // slow surprise baseline (novelty ref)
};
