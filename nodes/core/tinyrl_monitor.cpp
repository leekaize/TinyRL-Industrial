// =====================================================================
// tinyrl_monitor.cpp -- implementation. See tinyrl_monitor.h for design.
// =====================================================================
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <memory>
#include <vector>
#include <utility>
#include "tinyrl_monitor.h"
#include "condition_library.h"

// Layer-aware sparse-init densities (Stream-X v3 App. C, adapted). The
// global nonzero budget (1 - NET_SPARSITY) * total_weights is distributed
// over layers proportional to (fan_in + fan_out), capped at dense, with a
// connectivity FLOOR of >= 1 nonzero input per unit. The floor is the
// adaptation: the paper's budget alone still allows all-zero low-fan-in
// layers, and our first layer (fan_in = nf_) is exactly that case under
// the v2 uniform rule.
static std::vector<float> layerDensities(
    const std::vector<std::pair<int, int>>& shapes, float target_sparsity) {
  const size_t n = shapes.size();
  std::vector<long> cap(n), floornz(n), phi(n);
  size_t total = 0;
  long phisum = 0;
  for (size_t i = 0; i < n; ++i) {
    cap[i]    = (long)shapes[i].first * shapes[i].second;
    floornz[i] = shapes[i].second;               // 1 nonzero input per unit
    phi[i]    = shapes[i].first + shapes[i].second;
    total    += (size_t)cap[i];
    phisum   += phi[i];
  }
  long budget = lroundf((1.0f - target_sparsity) * (float)total);
  std::vector<long> nz(n);
  long used = 0;
  for (size_t i = 0; i < n; ++i) { nz[i] = floornz[i]; used += nz[i]; }
  while (used < budget) {                        // leftover: proportional, capped
    long given = 0;
    for (size_t i = 0; i < n && used < budget; ++i) {
      if (nz[i] >= cap[i]) continue;
      long share = (budget - used) * phi[i] / phisum;
      if (share < 1) share = 1;
      if (share > cap[i] - nz[i]) share = cap[i] - nz[i];
      nz[i] += share; used += share; given += share;
    }
    if (given == 0) break;
  }
  std::vector<float> density(n);
  for (size_t i = 0; i < n; ++i)
    density[i] = (float)nz[i] / (float)cap[i];
  return density;
}

void TinyRLConditionMonitor::buildNet() {
  if (cfg::SEED != 0) ag::manual_seed(cfg::SEED);   // deterministic init
  const std::vector<std::pair<int, int>> shapes = {
      {nf_, cfg::NET_HIDDEN}, {cfg::NET_HIDDEN, cfg::NET_HIDDEN}, {cfg::NET_HIDDEN, nf_}};
  const std::vector<float> dens = layerDensities(shapes, cfg::NET_SPARSITY);
  net_.clear();
  for (size_t i = 0; i < shapes.size(); ++i) {
    float sp = 1.0f - dens[i];
    const float smax = (shapes[i].first - 1.0f) / (float)shapes[i].first;
    if (sp > smax) sp = smax;                   // rounding guard for the floor
    net_.add(nn::Linear(shapes[i].first, shapes[i].second, sp));
    if (i + 1 < shapes.size()) {                // hidden activations only
      net_.add(nn::LayerNorm());
      net_.add(nn::LeakyReLU());
    }
  }
#if defined(MONITOR_USE_OBGD)
  opt_ = ObGD(cfg::OBGD_LR, cfg::OBGD_GAMMA, cfg::OBGD_LAMBDA, cfg::OBGD_KAPPA);
#else
  opt_ = StreamingOptimizer(cfg::MONITOR_LR, cfg::OBGD_GAMMA, cfg::OBGD_LAMBDA,
                            cfg::STREAM_BETA, cfg::STREAM_EPS);
#endif
  opt_.add_parameters(net_.layers());
  n_params_ = 0;
  for (auto& l : net_.layers())
    if (l->has_parameters())
      for (auto* p : l->get_parameters()) n_params_ += p->size();
}

void TinyRLConditionMonitor::resetSettle() {
  for (int f = 0; f < cfg::MAX_FEATURES; ++f)
    mot_fast_[f] = mot_slow_[f] = 0;
  calm_ = 0;
  settled_ = true;
  prev_settled_ = true;
  has_prev_rel_ = false;
  quiet_latch_ = false;
  sr_base_ = 1.0f;
  memset(prev_rel_, 0, sizeof(prev_rel_));
}

void TinyRLConditionMonitor::begin(const FeatureProfile& prof) {
  prof_ = &prof;
  nf_ = prof.n_features;
  fs_.begin(nf_);
  buildNet();
  resetSettle();
  prev_xn_ = ag::Matrix::Zeros(1, nf_);
  nm_mean_ = ag::Matrix::Zeros(1, nf_);
  nm_var_  = ag::Matrix::Ones(1, nf_);
  score_ = last_surprise_ = 1.0f;
  step_ = fit_n_ = 0;
  hi_ = lo_ = 0;
  learning_ = theta_ready_ = alarmed_ = false;
  has_prev_ = false;
  memset(last_raw_, 0, sizeof(last_raw_));
  Serial.printf("[RL] monitor ready: nf=%d hidden=%d params=%u (~%u KB optimizer state)\n",
                nf_, cfg::NET_HIDDEN, (unsigned)n_params_,
                (unsigned)((n_params_ * 3 * sizeof(float)) / 1024));
}

void TinyRLConditionMonitor::startLearning() {
  if (theta_ready_) {
    Serial.println("[RL] already monitoring - factory reset ('reset') to relearn");
    return;
  }
  fs_.begin(nf_);
  buildNet();                                  // fresh weights + traces
  resetSettle();
  obsnorm_.reset(new SampleMeanStd(1, nf_));
  prev_xn_.zero();
  has_prev_ = false;
  score_ = last_surprise_ = 1.0f;
  step_ = fit_n_ = 0;
  fit_mu_ = fit_m2_ = theta_ = 0;
  hi_ = lo_ = 0;
  alarmed_ = false;
  learning_ = true;
  Serial.println("[RL] learning STARTED - keep the machine at NORMAL load");
  Serial.printf("[RL] ~%lu s CAL (ranges), then ~%lu s theta fit\n",
                (unsigned long)(cfg::CAL_STEPS * cfg::FEED_PERIOD_MS / 1000),
                (unsigned long)(cfg::FIT_STEPS * cfg::FEED_PERIOD_MS / 1000));
}

void TinyRLConditionMonitor::factoryReset() {
  fs_.begin(nf_);
  buildNet();
  resetSettle();
  obsnorm_.reset();
  score_ = last_surprise_ = 1.0f;
  step_ = fit_n_ = 0;
  fit_mu_ = fit_m2_ = theta_ = 0;
  hi_ = lo_ = 0;
  learning_ = theta_ready_ = alarmed_ = false;
  has_prev_ = false;
}

void TinyRLConditionMonitor::attachLibrary(ConditionLibrary* lib) {
  lib_ = lib;
}

void TinyRLConditionMonitor::onWorldChange() {
  // Regime change (e.g. a just-labelled episode): keep the weights but
  // drop optimizer state (traces + leaky max) so old gradients and old
  // update scale don't leak into the new regime.
  opt_.step(0.0f, /*reset=*/true);
}

void TinyRLConditionMonitor::onFeedbackNormal() {     // desensitise
  // Operator says "this alarm is normal": theta must end ABOVE the score
  // they just saw, or the alarm re-fires immediately. The old fit-stats
  // cap (mu + 10*sd) was unreachable when sd came from 30 correlated
  // fit samples (vib node: cap ~0.95 vs quiet pops to 1.3+ - the bench
  // cycled alarm->normal 130 times, 2026-11-13). Repeated false alarms
  // keep ratcheting +5% over the seen score: the operator controls it.
  // No optimizer reset here: "nothing happened" is not a regime change,
  // and resetting mid-quiet destabilised the net each cycle.
  theta_ = fmaxf(theta_ * 1.10f, score_ * 1.05f);
  hi_ = lo_ = 0;
  alarmed_ = false;
}

void TinyRLConditionMonitor::onFeedbackFault() {      // sharpen
  float sd = sqrtf(fit_m2_ / (float)cfg::FIT_STEPS);
  float flr = (sd > 0.01f) ? fit_mu_ + 2.5f * sd : theta_ * 0.5f;
  theta_ = fmaxf(theta_ * 0.95f, flr);
  onWorldChange();
}

ag::Matrix TinyRLConditionMonitor::frozenNorm(const ag::Matrix& x) const {
  ag::Matrix out(1, nf_);
  for (int f = 0; f < nf_; ++f) {
    float sd = sqrtf(nm_var_(0, f) + 1e-8f);
    float z = (x(0, f) - nm_mean_(0, f)) / sd;
    if (z > cfg::Z_CLAMP) z = cfg::Z_CLAMP;         // out-of-fit regimes
    if (z < -cfg::Z_CLAMP) z = -cfg::Z_CLAMP;       // stay bounded (see cfg)
    out(0, f) = z;
  }
  return out;
}

// Live path while fitting: SampleMeanStd Welford + z-score. Math is
// identical to NormalizeObservation::normalize (var = p/(n-1), eps 1e-8)
// but the moments stay reachable for freezing + persistence.
static ag::Matrix welfordZScore(SampleMeanStd& st, const ag::Matrix& x) {
  st.update(x);
  return (x - st.getMean()) / (st.getVar() + 1e-8f).sqrt();
}

void TinyRLConditionMonitor::step(const float* raw, StepOut& out) {
  out.alarm_open = out.alarm_clear = false;
  memcpy(last_raw_, raw, nf_ * sizeof(float));

  // 1) condition space: Welford ranges until CAL done, then freeze once
  if (!fs_.armed()) {
    fs_.calibrateStep(raw);
    if (fs_.calReady()) {
      fs_.arm();
      for (int f = 0; f < nf_; ++f)
        Serial.printf("[COND] %-4s range %.4g .. %.4g\n",
                      prof_->features[f].name, fs_.lo(f), fs_.hi(f));
      Serial.println("[COND] ARMED - score statistics window starts");
    }
  }

  // 1b) settle gate: transitions never train, never alarm, never ask
  // (user requirement 2026-11-15: decide only on settled states). See
  // node_config.h for the per-feature rule. Warmup after arm/resume.
  if (fs_.armed()) {
    if (has_prev_rel_) {
      bool calm = true;
      for (int f = 0; f < nf_; ++f) {
        float d = fabsf(fs_.rel(f, raw[f]) - prev_rel_[f]);
        mot_fast_[f] += cfg::SETTLE_FAST_EMA * (d - mot_fast_[f]);
        mot_slow_[f] += cfg::SETTLE_SLOW_EMA * (d - mot_slow_[f]);
        float floor_v = cfg::SETTLE_FLOOR_UNITS * fs_.unit(f);
        if (mot_fast_[f] > cfg::SETTLE_MARGIN * fmaxf(mot_slow_[f], floor_v))
          calm = false;               // one moving feature vetoes the vector
      }
      calm_ = calm ? calm_ + 1 : 0;
      settled_ = calm_ >= cfg::SETTLE_CALM_N;
    }
    for (int f = 0; f < nf_; ++f) prev_rel_[f] = fs_.rel(f, raw[f]);
    has_prev_rel_ = true;
  }
  bool was_settled = prev_settled_;
  prev_settled_ = settled_;
  if (!theta_ready_) settled_ = true;   // baseline commissioning runs on a
                                        // settled machine (procedure)
  out.settled = settled_;

  // regime entry: a new stable state has begun -> drop optimizer scale
  // state. A stale leaky-max from the previous regime throttles the
  // re-adaptation every regime change needs (measured as the post-label
  // stall, run-155729, 2026-11-15). Traces die naturally within a few
  // steps; vmax_ decays with beta ~ 1 and must be cleared.
  if (theta_ready_ && settled_ && !was_settled) onWorldChange();

  // library match (any kind) at the quiet radius: shared by the training
  // gate and the B-rule (fs_ must be armed for rel())
  int km = (lib_ && fs_.armed())
               ? lib_->match(last_raw_, space(), cfg::QUIET_Q) : -1;

  // 2) TinyRL observation: running z-scores while fitting, frozen after
  ag::Matrix x(1, nf_);
  memcpy(x.data(), raw, nf_ * sizeof(float));
  ag::Matrix xn = theta_ready_ ? frozenNorm(x) : welfordZScore(*obsnorm_, x);

  // 3) one-step prediction + gated update (StreamQ update() pattern,
  //    minus the action: reward = -surprise, maximize it). The forward
  //    pass and surprise run every step so the score stays live through
  //    transitions; the WEIGHT UPDATE is gated:
  //      - mid-transition (unsettled): never (settle gate, 2026-11-15)
  //      - unknown regime (no library match): never - a novel condition
  //        keeps alarming until it is named (user policy 2026-11-15)
  //      - pre-theta (CAL/FIT): always (baseline commissioning)
  bool update = !theta_ready_ || (settled_ && km >= 0);
  if (has_prev_) {
    ag::Tensor s(prev_xn_, false);
    ag::Tensor sn(xn, false);
    ag::Tensor pred = net_.forward(s);          // (1,nf) predicts x_t
    ag::Tensor diff = pred - sn;
    ag::Tensor se   = ag::sum(diff * diff) * (1.0f / (float)nf_);
    opt_.zero_grad();
    ag::Tensor reward = -se;                    // backward on -se:
    reward.backward();                          // grads = -d(se)/d(theta)
    float surprise = se.item();
    if (update && theta_ready_) {
      // novelty-modulated rate (user requirement 2026-11-15): full speed
      // while surprise is high against its own slow baseline (new regime),
      // a small floor at the baseline (stability), never zero (slow drift
      // keeps creeping the baseline and stays trackable).
      // 2026-11-19: a state whose score sits INSIDE its own learned band
      // (the net already predicts it) refines at the floor rate only -
      // repeated full-rate entries into known states were the bench
      // "back and forth" degradation: each re-entry scrambled every other
      // regime's mapping a little, and the errors compounded.
      float base = fmaxf(sr_base_, 10.0f * cfg::SCORE_FLOOR);
      float frac = (surprise - base) / (cfg::NOVELTY_K * base);
      if (frac > 1.0f) frac = 1.0f;
      if (frac < cfg::LR_MIN_FRAC) frac = cfg::LR_MIN_FRAC;
      if (km >= 0 && lib_ && lib_->converged(km, score_))
        frac = cfg::LR_MIN_FRAC;
      opt_.set_lr(cfg::MONITOR_LR * frac);
    }
    if (update) opt_.step(-surprise, /*reset=*/false);  // delta < 0 ->
    reward.clear_graph();                       //    descent on surprise
    if (theta_ready_)
      sr_base_ += cfg::NOVELTY_EMA * (surprise - sr_base_);
    last_surprise_ = surprise;
  } else {
    last_surprise_ = 1.0f;
  }
  prev_xn_ = xn;
  has_prev_ = true;

  // 4) score EMA + phase machine
  score_ += cfg::SCORE_EMA * (last_surprise_ - score_);
  step_++;

  // per-condition score calibration (2026-11-19): settled matched steps
  // teach the condition its own surprise band (see step 5)
  if (km >= 0 && lib_ && settled_ && theta_ready_)
    lib_->observeScore(km, score_);

  if (!theta_ready_) {
    if (fs_.armed()) {                          // theta fit window
      fit_n_++;
      float d = score_ - fit_mu_;
      fit_mu_ += d / (float)fit_n_;
      fit_m2_ += d * (score_ - fit_mu_);
      if (fit_n_ >= cfg::FIT_STEPS) {
        theta_ = fit_mu_ + cfg::THETA_KSIG * sqrtf(fit_m2_ / (float)cfg::FIT_STEPS);
        nm_mean_ = obsnorm_->getMean().clone();
        nm_var_  = obsnorm_->getVar().clone();
        obsnorm_.reset();                       // freeze preprocessing
        theta_ready_ = true;
        learning_ = false;
        sr_base_ = fit_mu_;   // novelty baseline = the fit-window floor
        Serial.printf("[RL] theta=%.2f (mu=%.2f) - monitoring\n", theta_, fit_mu_);
      }
    }
    return;                                     // no alarms while fitting
  }

  // 5) threshold logic. ALARM MEANS "I DON'T KNOW THIS STATE". The
  // positional SCHMITT latch (in at QUIET_Q, out at MATCH_Q) covers any
  // KNOWN condition, normal OR fault (2026-11-19): the operator named it,
  // so a recurrence is not news - the condition name is surfaced while
  // quiet and that is the evidence. Labeled faults that re-alarm on every
  // visit were the operator-facing noise of the 2026-11-19 bench report
  // ("after some back and forth everything is wrong"). Unmatched states
  // gate on the global fit theta: unknown regimes keep alarming until
  // named (2026-11-15 policy), drift out of a known state re-alarms.
  // Per-condition SCORE bands as alarm thresholds were tried and rejected
  // the same day: the score at a state is NOT stationary across training
  // eras (the stopped score rose 1.2 -> ~6 after the speed states were
  // learned), so a band either false-alarms on every era shift or - with
  // an outlier gate - freezes and alarms forever. The band survives where
  // it is sound: converged-state detection for the LR floor (step 3) and
  // per-condition diagnostics in the library dump.
  if (!settled_) {
    lo_ = hi_ = 0;      // mid-transition: no alarm credit, no clear credit
  } else {
    if (quiet_latch_) {
      int mr = lib_ ? lib_->match(last_raw_, space(), cfg::MATCH_Q) : -1;
      if (mr < 0)
        quiet_latch_ = false;             // left the wide radius: re-earn quiet
    }
    if (!quiet_latch_ && km >= 0)
      quiet_latch_ = true;                // entered a known condition
    if (quiet_latch_) {
      lo_ = cfg::CLEAR_LO_N;  hi_ = 0;    // known: quiet
    } else if (score_ > theta_) {
      hi_++; lo_ = 0;                     // unknown and surprising: anomaly
    } else {
      hi_ = 0; lo_++;                     // unknown but quiet (tripwire's turf)
    }
  }

  if (!alarmed_ && hi_ >= cfg::ALARM_HI_N) {
    alarmed_ = true;
    out.alarm_open = true;
    Serial.printf("[RL] ALARM score=%.2f theta=%.2f\n", score_, theta_);
  } else if (alarmed_ && lo_ >= cfg::CLEAR_LO_N) {
    alarmed_ = false;
    out.alarm_clear = true;
    Serial.printf("[RL] clear  score=%.2f theta=%.2f\n", score_, theta_);
  }
}

const char* TinyRLConditionMonitor::stateString() const {
  if (!theta_ready_ && !learning_) return "idle";
  if (!theta_ready_) return "learn";
  return alarmed_ ? "alarm" : "ok";
}

float TinyRLConditionMonitor::confidence() const {
  if (!theta_ready_) return 0.0f;
  // matched: POSITIONAL confidence (1 at the centroid, 0 at the radius
  // edge) - the score ratio against the global theta read ~0 everywhere
  // except the commissioning state and made every known state look
  // unsure (2026-11-19 bench report)
  if (lib_ && fs_.armed()) {
    int idx = -1;
    float d = lib_->matchDist(last_raw_, fs_, cfg::QUIET_Q, &idx);
    if (idx >= 0)
      return fminf(1.0f, fmaxf(0.0f, 1.0f - d / cfg::QUIET_Q));
  }
  float conf = alarmed_ ? (score_ - theta_) / fmaxf(theta_, 0.1f)
                        : 1.0f - score_ / fmaxf(theta_, 0.01f);
  return fminf(1.0f, fmaxf(0.0f, conf));
}

void TinyRLConditionMonitor::exportState(State& s) const {
  s.score = score_; s.fit_mu = fit_mu_; s.fit_m2 = fit_m2_; s.theta = theta_;
  s.step = step_;
  s.cal = fs_.armed() && !theta_ready_;       // resume hint only
  s.theta_ready = theta_ready_; s.alarm = alarmed_;
  s.hi = hi_; s.lo = lo_;
  for (int f = 0; f < cfg::MAX_FEATURES; ++f) {
    s.fs_mu[f] = (f < nf_) ? fs_.mu(f) : 0;
    s.fs_unit[f] = (f < nf_) ? fs_.unit(f) : 0;
    s.fs_anchor[f] = (f < nf_) ? fs_.anchor(f) : 0;
    s.fs_lo[f] = (f < nf_) ? fs_.lo(f) : 0;
    s.fs_hi[f] = (f < nf_) ? fs_.hi(f) : 0;
    s.nm_mean[f] = (f < nf_) ? nm_mean_(0, f) : 0;
    s.nm_var[f]  = (f < nf_) ? nm_var_(0, f) : 1;
  }
  s.fs_armed = fs_.armed();
  s.fs_steps = fs_.calSteps();
}

bool TinyRLConditionMonitor::importState(const State& s) {
  if (!prof_ || nf_ <= 0 || nf_ > cfg::MAX_FEATURES) return false;
  resetSettle();                        // motion history invalid across boot
  score_ = s.score; fit_mu_ = s.fit_mu; fit_m2_ = s.fit_m2; theta_ = s.theta;
  step_ = s.step;
  hi_ = s.hi; lo_ = s.lo;
  theta_ready_ = s.theta_ready != 0;
  alarmed_ = s.alarm != 0;
  has_prev_ = false;

  if (theta_ready_) {
    // exact resume: frozen anchors/ranges + frozen normalizer are enough
    fs_.begin(nf_);
    for (int f = 0; f < nf_; ++f) {
      fs_.setMu(f, s.fs_mu[f]);
      fs_.setUnit(f, s.fs_unit[f]);
      fs_.setAnchor(f, s.fs_anchor[f]);
      fs_.setRange(f, s.fs_lo[f], s.fs_hi[f]);
    }
    fs_.forceArmed(s.fs_armed != 0, s.fs_steps);
    for (int f = 0; f < nf_; ++f) {
      nm_mean_(0, f) = s.nm_mean[f];
      nm_var_(0, f)  = s.nm_var[f];
    }
    obsnorm_.reset();
    learning_ = false;
    sr_base_ = fmaxf(s.score, 10.0f * cfg::SCORE_FLOOR);   // novelty ref
  } else if (s.cal != 0) {
    // saved mid-learning: restarting CAL automatically beats v3's
    // "press 's' again" - the network keeps whatever it had learned.
    fs_.begin(nf_);
    obsnorm_.reset(new SampleMeanStd(1, nf_));
    score_ = last_surprise_ = 1.0f;
    fit_mu_ = fit_m2_ = 0;
    fit_n_ = 0;
    learning_ = true;
  } else {
    // idle snapshot
    fs_.begin(nf_);
    obsnorm_.reset();
    learning_ = false;
  }
  return true;
}
