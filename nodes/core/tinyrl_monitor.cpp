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

// The band-gated eras (MONITOR_REL_OBS = rel levels, MONITOR_COND_OBS =
// per-condition centered residuals) share everything downstream of the
// observation: the active-gate selection (per-condition band vs global
// theta), the band-driven rate rule, the band-feeding fit-gate + ratchet,
// grace, and the threshold logic. Only step 2 (observation construction)
// and the era tag differ. The legacy z era keeps its own paths.
#if defined(MONITOR_REL_OBS) || defined(MONITOR_COND_OBS)
#define MONITOR_BAND_ERAS 1
#endif

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
  rel_hist_n_ = 0;
  rel_hist_i_ = 0;
  settled_ = true;
  prev_settled_ = true;
  has_prev_rel_ = false;
  quiet_latch_ = false;
  sr_base_ = 1.0f;
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
  Serial.printf("%s[RL] monitor ready: nf=%d hidden=%d params=%u (~%u KB optimizer state)\n", node_tag_,
                nf_, cfg::NET_HIDDEN, (unsigned)n_params_,
                (unsigned)((n_params_ * 3 * sizeof(float)) / 1024));
}

void TinyRLConditionMonitor::startLearning() {
  if (theta_ready_) {
    Serial.printf("%s[RL] already monitoring - factory reset ('reset') to relearn\n",
                  node_tag_);
    return;
  }
  fs_.begin(nf_);
  buildNet();                                  // fresh weights + traces
  resetSettle();
  obsnorm_.reset(new SampleMeanStd(1, nf_));
  prev_xn_.zero();
  has_prev_ = false;
  cond_frame_ = -2;                            // fresh frame history (cond era)
  sticky_km_ = -1;                             // fresh match history (band eras)
  km_prev_ = -1;
  prev_sticky_ = prev_grace_ = false;
  score_ = last_surprise_ = 1.0f;
  step_ = fit_n_ = 0;
  fit_mu_ = fit_m2_ = theta_ = 0;
  hi_ = lo_ = 0;
  alarmed_ = false;
  learning_ = true;
  Serial.printf("%s[RL] learning STARTED - keep the machine at NORMAL load\n",
                node_tag_);
  Serial.printf("%s[RL] ~%lu s CAL (ranges), then ~%lu s theta fit\n", node_tag_,
                (unsigned long)(cfg::CAL_STEPS * cfg::FEED_PERIOD_MS / 1000),
                (unsigned long)(cfg::FIT_STEPS * cfg::FEED_PERIOD_MS / 1000));
}

void TinyRLConditionMonitor::factoryReset() {
  fs_.begin(nf_);
  buildNet();
  resetSettle();
  obsnorm_.reset();
  cond_frame_ = -2;
  sticky_km_ = -1;
  km_prev_ = -1;
  prev_sticky_ = prev_grace_ = false;
  alarm_acc_ = 0;
  pos_acc_ = 0;
  pos_breach_n_ = 0;
  score_breach_n_ = 0;
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

void TinyRLConditionMonitor::voidPosEvidence() {
  pos_acc_ = 0.0f;   // the operator named the state: the accumulated
                     // positional evidence is resolved. Without this the
                     // reservoir (capped during the unknown window, leak
                     // 0.05/step) re-fired every borderline breach moment
                     // for ~28 steps after the label
  pos_breach_n_ = 0; // the standing proof restarts with the evidence
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
  // Ablation (ABL_SETTLE_GATE=0): the gate never settles the machine
  // down - decisions and training run mid-move. The regime-entry
  // optimizer reset and the re-entry grace arm on the settle EDGE
  // (below), so both go quiet with it.
#if ABL_SETTLE_GATE
  if (fs_.armed()) {
    if (has_prev_rel_) {
      // net displacement over the window (node_config.h): the state's own
      // wander cancels, a machine ramp accumulates, a knock returns
      for (int f = 0; f < nf_; ++f)
        rel_hist_[rel_hist_i_][f] = fs_.rel(f, raw[f]);
      rel_hist_i_ = (rel_hist_i_ + 1) % cfg::SETTLE_WINDOW_N;
      if (rel_hist_n_ < cfg::SETTLE_WINDOW_N) ++rel_hist_n_;
      settled_ = false;
      if (rel_hist_n_ == cfg::SETTLE_WINDOW_N) {
        settled_ = true;                    // every feature must stay put
        for (int f = 0; f < nf_ && settled_; ++f) {
          float now_r =
            rel_hist_[(rel_hist_i_ + cfg::SETTLE_WINDOW_N - 1) %
                      cfg::SETTLE_WINDOW_N][f];
          float old_r = rel_hist_[rel_hist_i_][f];  // oldest = next overwrite
          if (fabsf(now_r - old_r) > cfg::SETTLE_NET_DRIFT) settled_ = false;
        }
      }
    }
    has_prev_rel_ = true;
  }
#endif  // ABL_SETTLE_GATE
  bool was_settled = prev_settled_;
  prev_settled_ = settled_;
  if (theta_ready_ && settled_ != was_settled) {
    // [WHY] the settle gate flipped: name the feature carrying the worst
    // net drift over the window relative to the bound
    int wf = -1; float wd = 0;
    if (rel_hist_n_ == cfg::SETTLE_WINDOW_N) {
      for (int f = 0; f < nf_; ++f) {
        float now_r =
          rel_hist_[(rel_hist_i_ + cfg::SETTLE_WINDOW_N - 1) %
                    cfg::SETTLE_WINDOW_N][f];
        float d = fabsf(now_r - rel_hist_[rel_hist_i_][f]);
        if (d > wd) { wd = d; wf = f; }
      }
    }
    const char* wfn = (wf >= 0 && prof_) ? prof_->features[wf].name : "?";
    if (settled_)
      Serial.printf("%s[WHY] settled: net drift <= %.1f decades over %d steps"
                    " (worst %s %.2f) - decisions resume\n", node_tag_,
                    (double)cfg::SETTLE_NET_DRIFT, cfg::SETTLE_WINDOW_N,
                    wfn, (double)wd);
    else
      Serial.printf("%s[WHY] UNSETTLED: %s net drift %.2f > %.1f decades"
                    " over %d steps - no train/alarm/ask mid-move\n", node_tag_,
                    wfn, (double)wd, (double)cfg::SETTLE_NET_DRIFT,
                    cfg::SETTLE_WINDOW_N);
  }
  if (!theta_ready_) settled_ = true;   // baseline commissioning runs on a
                                        // settled machine (procedure)
  out.settled = settled_;

  // regime entry: a new stable state has begun -> drop optimizer scale
  // state. A stale leaky-max from the previous regime throttles the
  // re-adaptation every regime change needs (measured as the post-label
  // stall, run-155729, 2026-11-15). Traces die naturally within a few
  // steps; vmax_ decays with beta ~ 1 and must be cleared.
  if (theta_ready_ && settled_ && !was_settled) {
    Serial.println("[WHY] regime entry: transition finished - optimizer"
                   " traces + leaky-max dropped so the new regime can"
                   " re-adapt");
    onWorldChange();
    // re-entry grace, ALL eras (2026-11-22): the net's surprise is not
    // evidence while it re-adapts after the optimizer reset. The z-era
    // churn rode on the old settle gate staying unsettled longer; the
    // net-drift gate exposed the bare 5-step streak.
    reentry_grace_ = cfg::REENTRY_GRACE_N;
  }
  // the grace decrements HERE, era-neutral: the old decrement sat in the
  // band-era arm, so the z era never counted it down and a held streak
  // (the 2026-11-22 z-era churn fix) would never release
  if (reentry_grace_ > 0) --reentry_grace_;

  // library match (any kind) at the quiet radius: shared by the training
  // gate and the B-rule (fs_ must be armed for rel())
  int km = (lib_ && fs_.armed())
               ? lib_->match(last_raw_, space(), cfg::QUIET_Q) : -1;
  // decision-trace captures (era-neutral; defaults are the z-era values)
  bool  sticky_held = false;
  float ev_used = 0.0f;
  bool  trained_step = false;
  float lr_frac_used = -1.0f;
  int   frame_cm = -1;
  bool  in_grace_now = false;
  float d_match_now = -1.0f;
#ifdef MONITOR_BAND_ERAS
  // Radius-edge stickiness (hardware finding 2026-11-21): the rig's slow
  // meter drift parked a steady state exactly at the QUIET_Q edge, the
  // match flickered out, and every flicker judged the KNOWN state against
  // the stopped-scale fit theta (ev = score/theta - 1 ~ +15 in one
  // settled step -> instant re-alarm). Hold the last matched condition
  // while it is still within MATCH_Q; first-time matches are unaffected,
  // so genuinely new states keep their alarm path, and drift beyond
  // MATCH_Q still asks (refine() re-centers on the label).
  // Ablation (ABL_STICKY_HOLD=0): no hold - every step past QUIET_Q is
  // unknown again and re-judged against the active gate; the tenure
  // release and its burn go with it. sticky_km_ still tracks the last
  // GENUINE match below, so the [WHY] lines keep their semantics.
#if ABL_STICKY_HOLD
  if (km < 0 && sticky_km_ >= 0 && lib_ && !hold_burned_) {
    float d_hold = lib_->distTo(sticky_km_, last_raw_, space());
    if (d_hold >= 0 && d_hold <= cfg::MATCH_Q) {
      km = sticky_km_; sticky_held = true;
      if (!prev_sticky_ && theta_ready_)
        Serial.printf("%s[WHY] match held (sticky): '%s' d=%.2f is past the"
                      " alarm edge %.1f but within MATCH_Q %.1f - radius-edge"
                      " drift, still this state\n", node_tag_,
                      lib_->at(km).id, (double)d_hold,
                      (double)cfg::QUIET_Q, (double)cfg::MATCH_Q);
      // Sustained-tenure release (node_config.h STICKY_RELEASE_N): the
      // hold is for radius-edge FLICKERS of the state's own rows - they
      // re-acquire within a few steps. A yardstick-grown radius can span
      // the gap to a NEIGHBOURING state, and the hold then carries the
      // foreign rows forever - no alarm, no ask, the state never learns
      // (the speed_2 first visit sat d~3.5-4 inside the grown 'speed_1'
      // radius). Tenure is the discriminator, not the score: the score
      // band adapts to ANY sustained floor (foreign rows included), but
      // every recorded transition ramp completes inside 80 settled steps
      // while a state visit sustains for its whole block. Release and
      // burn: the unknown path alarms and asks, the operator names the
      // state, and the genuine match re-arms the hold.
      if (settled_ && theta_ready_) {
        if (++sticky_hold_n_ >= cfg::STICKY_RELEASE_N) {
          Serial.printf("%s[WHY] sticky hold on '%s' released: %d settled"
                        " steps held without a genuine match - a"
                        " radius-edge hold cannot span a state change\n", node_tag_,
                        lib_->at(sticky_km_).id, cfg::STICKY_RELEASE_N);
          km = -1;
          sticky_held = false;
          sticky_km_ = -1;               // burned until a genuine match
          hold_burned_ = true;
          sticky_hold_n_ = 0;
        }
      }
    }
  }
#endif  // ABL_STICKY_HOLD
  // distance for the FINAL km (sticky included) - the [WHY] lines use it
  d_match_now = (km >= 0 && lib_)
                    ? lib_->distTo(km, last_raw_, space()) : -1.0f;
  if (theta_ready_ && lib_ && lib_->size() > 0) {
    // [WHY] the matched condition changed this step. 'left' fires once,
    // on the transition into unknown (km_prev_), not on every step of the
    // gap - the sticky retention below keeps sticky_km_ valid through it.
    if (sticky_km_ < 0 && km >= 0) {
      int wf = lib_->worstFeature(km, last_raw_, space());
      Serial.printf("%s[WHY] matched '%s': worst feature %s d=%.2f <= QUIET_Q"
                    " %.1f\n", node_tag_, lib_->at(km).id,
                    (wf >= 0 && prof_) ? prof_->features[wf].name : "?",
                    (double)d_match_now, (double)cfg::QUIET_Q);
    } else if (km < 0 && km_prev_ >= 0) {
      int wf = lib_->worstFeature(sticky_km_, last_raw_, space());
      float d_left = lib_->distTo(sticky_km_, last_raw_, space());
      Serial.printf("%s[WHY] unknown: left '%s' - %s d=%.2f > MATCH_Q %.1f"
                    " (drift beyond the hold radius; judged against the"
                    " nearest band / fit theta until named)\n", node_tag_,
                    lib_->at(sticky_km_).id,
                    (wf >= 0 && prof_) ? prof_->features[wf].name : "?",
                    (double)d_left, (double)cfg::MATCH_Q);
    } else if (sticky_km_ >= 0 && km >= 0 && km != sticky_km_) {
      Serial.printf("%s[WHY] switched '%s' -> '%s' (nearest wins: d=%.2f vs"
                    " %.2f)\n", node_tag_, lib_->at(sticky_km_).id, lib_->at(km).id,
                    (double)d_match_now,
                    (double)lib_->distTo(sticky_km_, last_raw_, space()));
    }
  }
  // Radius-edge stickiness, RETENTION (2026-11-21 rig replay finding): the
  // hold is distance-gated EVERY step (d_hold <= MATCH_Q above), so keeping
  // the last matched condition across unmatched steps costs nothing when the
  // vector truly leaves - but a multi-step transition gap (every motor
  // start) wipes a one-step memory, and the settled vector on the far side
  // (back within 4.0 of 'speed_1') was then judged against the stopped-scale
  // fit theta: acc +17 in one step, the tour's revisit alarm. First-time
  // matches are unaffected, so genuinely new states keep their alarm path.
  // Scoped to the cond era (the deployed firmware, rig-driven); the rel
  // paper arm keeps its documented one-step behavior.
#ifdef MONITOR_COND_OBS
  if (km >= 0 && !sticky_held) {
    sticky_km_ = km;
    hold_burned_ = false;              // a genuine match re-arms the hold
    sticky_hold_n_ = 0;
  }
#else
  sticky_km_ = km;
#endif
  prev_sticky_ = sticky_held;
  km_prev_ = km;
  // the ACTIVE gate for this step, needed by the rate rule (step 3) and
  // the threshold logic (step 5): the condition's own band once mature,
  // the global fit theta otherwise (an immature band falls back inside
  // scoreTheta). Ablation (ABL_PERCOND_BAND=0): every state is judged by
  // the global fit theta.
#if ABL_PERCOND_BAND
  float gate = (km >= 0 && lib_) ? lib_->scoreTheta(km, theta_) : theta_;
#else
  float gate = theta_;
#endif
#endif
  // cond-era per-step evidence, declared era-neutral so the shared
  // threshold logic below compiles unchanged in every build
  float ragg = 0.0f;
  float gate_r = -1.0f;
  int   pos_ref_km = -1;
  bool  score_reanchored = false;   // cond era: the floor-excess re-anchor
                                    // replaced the score band this step -
                                    // the accumulator void lands after the
                                    // step's charge (era-neutral so the
                                    // shared accumulator compiles everywhere)
#ifdef MONITOR_COND_OBS
  // Positional aggregate against the MATCHED condition's own frame: the
  // one-step predictor converges to identity in a static regime
  // (pred(x_{t-1}) ~= x_t, in ANY coordinate frame), so a STATIC offset is
  // invisible to the score gate once settled - the stage-H in-radius miss
  // in all three eras' score paths. The residual sum sees it: quiet steps
  // sit near chi2(nf), the 2.5-sigma-everywhere fault at nf * 6.25.
  // Ablation (ABL_POS_EVIDENCE=0): no positional channel - ragg/gate_r
  // stay dead (0/-1), the pos fire arm never arms, the excursion
  // re-anchor and the identity-adaptation guard (which read gate_r) fall
  // dormant, and an in-radius static offset is invisible again.
  if (km >= 0 && lib_) {
#if ABL_POS_EVIDENCE
    const Condition& pc = lib_->at(km);
    for (int f = 0; f < nf_; ++f) {
      float unit = pc.u[f] > 0 ? pc.u[f] : space().unit(f);
      float d = (space().rel(f, last_raw_[f]) - pc.c[f]) / unit;
      ragg += d * d;
    }
    gate_r = lib_->posTheta(km);
    pos_ref_km = km;
#endif
  } else if (lib_ && lib_->size() > 0) {
    // Unknown state (2026-11-21 rig replay finding, handoff 7.4 "global
    // theta cold start"): judging an unmatched vector against the fit
    // theta fitted at ONE state false-alarms radius-edge drift the nearest
    // condition's own score band already calls normal (score 3.15 vs
    // theta 0.77 = 4.1x on the rig). Gate it against the NEAREST
    // condition's own matured bands instead; the fit theta remains the
    // fallback while they form. A genuinely novel state still accumulates
    // - its residuals sit far outside the nearest centroid's positional
    // band - detection slows from one settled step to a few, false alarms
    // stop.
    float nd = -1.0f;
    int nc = lib_->nearest(last_raw_, space(), &nd);
    if (nc >= 0) {
      const Condition& uc = lib_->at(nc);
#if ABL_POS_EVIDENCE
      for (int f = 0; f < nf_; ++f) {
        float unit = uc.u[f] > 0 ? uc.u[f] : space().unit(f);
        float d = (space().rel(f, last_raw_[f]) - uc.c[f]) / unit;
        ragg += d * d;
      }
      gate_r = lib_->posTheta(nc);
      pos_ref_km = nc;
#endif
#if ABL_PERCOND_BAND
      // (the score half of the unknown-path judgment; the positional
      // half above is ABL_POS_EVIDENCE) Gate the unmatched vector against
      // the NEAREST condition's own matured score band; the fit theta
      // remains the fallback while bands form. A genuinely novel state
      // still accumulates - its residuals sit far outside the nearest
      // centroid's bands - detection slows from one settled step to a
      // few, false alarms stop.
      gate = lib_->scoreTheta(nc, theta_);
#else
      (void)uc;
#endif
    }
  }
  // A frame switch (the label arriving mid-settled-state, or a direct A->B
  // match change) rescales the agent's input frame. Without dropping the
  // optimizer's scale state the prediction shock poisons the leaky-max and
  // the heal takes thousands of steps (bench: load_b stuck at 0.91 after a
  // 4000-step window, 2026-11-21). Same remedy as regime entry; the last
  // matched frame is remembered so a radius-edge flicker (k -> -1 -> k)
  // does not fire it.
  if (theta_ready_ && settled_ && km >= 0 && km != cond_frame_) {
    if (cond_frame_ >= 0)
      Serial.printf("%s[WHY] frame switch %s -> %s mid-settled: optimizer"
                    " traces + leaky-max dropped (the label rescales the"
                    " input frame)\n", node_tag_,
                    (cond_frame_ < lib_->size()) ? lib_->at(cond_frame_).id
                                                 : "?",
                    lib_->at(km).id);
    onWorldChange();
  }
  if (km >= 0) cond_frame_ = km;
#endif

  // 2) TinyRL observation
  ag::Matrix x(1, nf_);
  memcpy(x.data(), raw, nf_ * sizeof(float));
#if defined(MONITOR_REL_OBS)
  // rel-obs era: the agent sees anchored-ratio coordinates (feature_space.h
  // rel()) instead of frozen z. Frozen z pins every running regime at the
  // +-5 clamp - one input point, so the prediction error cannot tell
  // regimes or faults apart (bench finding 2026-11-20). rel() needs the
  // armed CAL moments; before arm() the commissioning regime's rel
  // position IS ~0, so zeros are continuous across the arm.
  ag::Matrix xn(1, nf_);
  for (int f = 0; f < nf_; ++f)
    xn(0, f) = fs_.armed() ? fs_.rel(f, raw[f]) : 0.0f;
#elif defined(MONITOR_COND_OBS)
  // per-cond centered era: the agent sees RESIDUALS in the matched
  // condition's own units, obs = (rel - c_matched) / u_matched. Every
  // condition's training manifold sits at 0 in this space, so a static
  // offset (the in-radius fault) is off-manifold no matter which regimes
  // were learned, and the conditional-mean predictor regresses to 0 - the
  // surprise becomes the squared distance from the regime's own centre.
  // Unknown states center on the NEAREST condition (>= 3 units off by
  // definition of a match failure - the tripwire). Before the first
  // condition exists (the whole commissioning window) the frame is CAL-
  // unit normalized rel, and the auto-registered op_stopped baseline has
  // centroid 0 + CAL fallback units, so the fit -> monitoring handover is
  // continuous. u=0 (no batch) falls back to the CAL unit, same rule as
  // match().
  ag::Matrix xn(1, nf_);
  if (!fs_.armed()) {
    for (int f = 0; f < nf_; ++f) xn(0, f) = 0.0f;
  } else {
    int cm = -1;
    if (lib_ && lib_->size() > 0)
      cm = (km >= 0) ? km : lib_->nearest(last_raw_, space(), nullptr);
    frame_cm = cm;   // decision trace: whose frame the agent is judged in
    if (cm >= 0) {
      const Condition& cc = lib_->at(cm);
      for (int f = 0; f < nf_; ++f) {
        float unit = cc.u[f] > 0 ? cc.u[f] : fs_.unit(f);
        float r = (fs_.rel(f, raw[f]) - cc.c[f]) / unit;
        if (r >  cfg::RESID_CLAMP) r =  cfg::RESID_CLAMP;
        if (r < -cfg::RESID_CLAMP) r = -cfg::RESID_CLAMP;
        xn(0, f) = r;
      }
    } else {
      for (int f = 0; f < nf_; ++f)
        xn(0, f) = fs_.rel(f, raw[f]) / fmaxf(fs_.unit(f), 1e-6f);
    }
  }
#else
  // z era: running z-scores while fitting, frozen after theta fit
  ag::Matrix xn = theta_ready_ ? frozenNorm(x) : welfordZScore(*obsnorm_, x);
#endif

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
#ifdef MONITOR_BAND_ERAS
      // Band-gated eras: the rate is band-driven. A matched state whose
      // score sits inside its own band is predicted - refine at the floor
      // rate (the 2026-11-19 anti-scrambling rule, now sound because bands
      // are per-regime stationary). A matched state ABOVE its gate - first
      // learning pass, re-entry healing after interference, or a real
      // excursion - re-learns at RELEARN_FRAC: full rate burst-thrashed
      // (the leaky-max keeps burst capacity for ~beta^-1 steps and the
      // score self-oscillated around the band, stage H 2026-11-20) while
      // the floor rate made interference heals take hours at 1 Hz.
      if (km >= 0 && score_ > gate)
        frac = fmaxf(frac, cfg::RELEARN_FRAC);
      if (km >= 0 && lib_ && lib_->converged(km, score_))
        frac = cfg::LR_MIN_FRAC;
#else
      if (km >= 0 && lib_ && lib_->converged(km, score_))
        frac = cfg::LR_MIN_FRAC;
#endif
      opt_.set_lr(cfg::MONITOR_LR * frac);
      lr_frac_used = frac;   // decision trace: the rate this step used
    }
    if (update) opt_.step(-surprise, /*reset=*/false);  // delta < 0 ->
    reward.clear_graph();                       //    descent on surprise
    trained_step = update;
    if (theta_ready_)
      sr_base_ += cfg::NOVELTY_EMA * (surprise - sr_base_);
    last_surprise_ = surprise;
  } else {
#ifdef MONITOR_BAND_ERAS
    last_surprise_ = 0.0f;   // no prediction evidence yet: an initial spike
#else                       // would alarm against a band that is not formed
    last_surprise_ = 1.0f;   // z era keeps its legacy first-step value
#endif
  }
  prev_xn_ = xn;
  has_prev_ = true;

  // 4) score EMA + phase machine
  score_ += cfg::SCORE_EMA * (last_surprise_ - score_);
  step_++;

  // per-condition score calibration (2026-11-19): settled matched steps
  // teach the condition its own surprise band (see step 5). Sticky-held
  // steps are excluded (2026-11-22 real-data tour): a held foreign row
  // must not teach the band, the positional band, the yardstick or the
  // centroid - the hold says "probably still this state", never "learn
  // from me" (the held speed_2 rows otherwise re-formed speed_1's bands
  // at speed_2's floor and merged the two conditions).
  if (km >= 0 && lib_ && settled_ && theta_ready_ && !sticky_held) {
#ifdef MONITOR_BAND_ERAS
    const Condition& bc = lib_->at(km);
#if defined(MONITOR_COND_OBS)
    // Stationarity gate, SCALE-FREE and self-referential: the centered
    // frame rescales every regime to its own sigma, so the commissioning
    // floor (fit_mu + 3 fit_sd, FIT-frame units) is meaningless for other
    // conditions - the first cond bench run matured NO band in 4000 steps
    // because of it. The test below asks whether the score EMA and the raw
    // surprise AGREE (within 30%): at a converged regime they coincide; a
    // decaying label transient or a rising excursion pulls the raw value
    // far from its EMA. The first replacement attempt used sr_base_ (the
    // slow surprise EMA) as the baseline, but on the rig EVERY label
    // follows an alarm, whose transient inflates sr_base_ itself - the
    // bench then matured load_b's band on alarm-level scores (gate ~35,
    // stage H 2026-11-21). The score-vs-surprise gap cannot be
    // contaminated from outside: it measures the score's own stability.
    bool quiet_enough = fabsf(score_ - last_surprise_)
                        < 0.3f * fmaxf(score_, last_surprise_);
#else
    // Only steps as quiet as the commissioning floor teach the band: the
    // post-label learning transient must not raise the state's own future
    // alarm line. fit_mu_ + 3 fit_sd is the fit window's 3-sigma envelope.
    // A regime genuinely noisier than that never matures its band and
    // stays on the operator-gated global theta (documented escape hatch).
    // Feeding on "just below global theta" instead matured the band on
    // hovering transient scores, tripped the converged LR floor at the
    // alarm edge, and froze the net mid-descent (bench stage H chatter,
    // 2026-11-20).
    float fit_sd = sqrtf(fit_m2_ / (float)cfg::FIT_STEPS);
    bool quiet_enough = score_ <= fit_mu_ + 3.0f * fit_sd;
#endif
#if defined(MONITOR_COND_OBS)
    // Formation phase (s_n < MIN_N) feeds unconditionally inside the
    // stationarity gate. The rel-era ratchet assumed a DECAYING score and
    // followed it down; at a converged regime the score is FLAT noise, so
    // a first-sample-frozen mu + tiny sigma blocks every normal draw after
    // it (bench: load_a s_n froze < 16 in 600 settled steps, 2026-11-21 -
    // and the stuck band kept the state in permanent grace). Once mature,
    // the ratchet + observeScore's outlier gate guard the band.
    bool in_band = (bc.s_n < cfg::PERCOND_MIN_N) ||
                   (score_ <= bc.s_mu + 2.0f * sqrtf(bc.s_m2 / (float)bc.s_n));
#else
    bool in_band = (bc.s_n == 0) ||
                   (score_ <= bc.s_mu + 2.0f * sqrtf(bc.s_m2 / (float)bc.s_n));
#endif
    // in_band is a RATCHET: the band follows the score down as the net
    // converges, but a decaying tail can never pull mu up. The first rule
    // alone let a 0.008 -> 0.001 decay tail feed the band all the way to
    // the alarm edge, which masked in-radius faults afterwards (stage H,
    // 2026-11-20). observeScore's own outlier gate keeps guarding once
    // mature.
    const unsigned sn_before = bc.s_n;
    if (quiet_enough && in_band) lib_->observeScore(km, score_);
    if (sn_before + 1 == cfg::PERCOND_MIN_N &&
        bc.s_n == cfg::PERCOND_MIN_N)     // print ONCE, on the transition
      Serial.printf("%s[WHY] '%s' score band MATURED (16 samples): gate ="
                    " mu+3sd = %.3f replaces the global theta %.3f for this"
                    " state\n", node_tag_,
                    bc.id, (double)lib_->scoreTheta(km, 0.0f),
                    (double)theta_);
#if defined(MONITOR_COND_OBS)
    // positional band: geometry, not prediction - the residual aggregate
    // does not decay after a label, so no transient gate is needed here;
    // observePos's outlier guard keeps guarding once mature. Feeds on
    // every settled matched step, so the band matures within the research
    // budget (60-300 samples per new condition).
    const unsigned rn_before = bc.r_n;
    lib_->observePos(km, ragg);
    if (rn_before + 1 == cfg::PERCOND_MIN_N &&
        bc.r_n == cfg::PERCOND_MIN_N)     // print ONCE, on the transition
      Serial.printf("%s[WHY] '%s' positional band MATURED (16 samples):"
                    " gate_r = %.2f - static offsets are now visible to the"
                    " evidence\n", node_tag_, bc.id, (double)lib_->posTheta(km));
#endif
    // Yardstick + centroid adaptation (cfg::YARDSTICK_ALPHA, 2026-11-21
    // rig replay): positions, not scores, teach the yardstick - gated on
    // settled matched steps only, so the post-label score decay (score and
    // surprise disagree while the EMA lands) still calibrates the spread.
    // A fault's yardstick learns its own honest spread; its centroid is
    // the operator's label and never moves. Cond era only: the rel paper
    // arm keeps its documented behavior.
#ifdef MONITOR_COND_OBS
    // Identity adaptation runs ONLY on genuinely matched steps (2026-11-22
    // real-data tour): a sticky-held step says "probably still this state"
    // - it keeps the alarm quiet - but it must not TEACH: on a machine
    // transition the held rows walk toward the destination, the yardstick
    // compounds ~15%/held-step across the ramp (the hold itself bounds the
    // observed offset at 4 batch-units, so u can ratchet freely), the
    // grown radius then swallows foreign rows, and the centroid follows -
    // 20 churn cycles merged speed_1 and speed_2 (both centroids d~0.05
    // from the same rows). The rates separate the two legitimate cases:
    // thermal drift (0.0005 decades/step) tracks inside the match radius
    // on matched steps (yardstick lag ~0.5 units, never held), while a
    // setpoint ramp (0.009 decades/step) outruns the yardstick and falls
    // to unknown - where only the neighborhood's UNITS-only growth (below)
    // may widen radii, never move identities.
    if (!sticky_held &&
        !(gate_r > 0.0f && ragg > cfg::POS_ADAPT_BLOCK * gate_r)) {
      lib_->adaptUnits(km, last_raw_, space(), cfg::YARDSTICK_ALPHA);
      if (lib_->at(km).kind == COND_NORMAL)
        lib_->adaptCentroid(km, last_raw_, space(), cfg::YARDSTICK_ALPHA);
    }
    // else: the geometry says foreign (node_config.h POS_ADAPT_BLOCK).
    // An in-radius fault IS matched - absorbing it here walked speed_2's
    // centroid onto the phase_loss rows (ragg 18.9 -> 0.01) and window 2
    // of the same fault became invisible. A subsided excursion re-anchors
    // the band first, which reopens adaptation on the honest band.
#endif
#else
    lib_->observeScore(km, score_);
#endif
  }
#ifdef MONITOR_COND_OBS
  // Yardstick from the neighborhood (2026-11-21 rig replay, handoff 7.3):
  // between-visit instrument drift parks a KNOWN state just outside the
  // tight batch-derived radius for whole minutes; with the score band still
  // forming, the unknown judgment falls back to the stopped-scale fit theta
  // and re-alarms - the tour's speed_1 revisit alarm latched through the
  // whole phase_loss stage and starved the fault of its own edge. While the
  // nearest NORMAL condition is within YARDSTICK_FAR and the step is quiet
  // (score and surprise agree), grow its yardstick toward the observed
  // offset: the state re-matches, grace flushes the accumulator, and the
  // band matures on the recovered quiet steps. A fault cannot abuse this:
  // its offset exceeds YARDSTICK_FAR in one step, and FAULT-kind neighbors
  // never adapt from outside their radius.
  // NOT while alarmed (2026-11-21 suite finding): an alarmed plateau is the
  // system saying "I don't know this state" - absorbing it into a normal
  // neighbor's centroid would swallow the very state the operator must name
  // (the sim_test shift_b case: load_a walked onto the unnamed load_b in
  // 50 alarmed steps). Quiet-unmatched steps only - there the tripwire
  // watches and the re-match heals.
  // Ablation (ABL_YARDSTICK_GROWTH=0, run with ABL_STICKY_HOLD=0 as the
  // drift-recovery family): no between-visit drift recovery - a known
  // state parked outside its tight batch radius stays unknown and asks.
#if ABL_YARDSTICK_GROWTH
  if (km < 0 && settled_ && theta_ready_ && !alarmed_ && lib_ &&
      lib_->size() > 0) {
    // NOTE: no score-vs-surprise agreement gate here. In the centered
    // frame the surprise IS the squared distance to the nearest centroid,
    // so on a drift plateau it is huge while the score EMA lags - the
    // agreement test fails exactly when the adaptation is needed. The
    // settle gate (settled_) is the motion gate; the kind/distance gates
    // bound the blast radius.
    float nd = -1.0f;
    int nc = lib_->nearest(last_raw_, space(), &nd);
    if (nc >= 0 && lib_->at(nc).kind == COND_NORMAL &&
        nd <= cfg::YARDSTICK_FAR) {
      // UNITS only (2026-11-22 real-data tour): the far-drift fix needs the
      // state to RE-MATCH, and a grown radius does that. Growing the
      // centroid here let machine-transition ramps (quiet-unmatched,
      // within YARDSTICK_FAR of the origin condition for a few steps each)
      // drag the identity toward the destination - the same merge the held
      // matched path caused before its !sticky_held gate. The centroid is
      // the identity: it learns only from genuinely matched steps.
      lib_->adaptUnits(nc, last_raw_, space(), cfg::YARDSTICK_ALPHA);
    }
  }
#endif  // ABL_YARDSTICK_GROWTH
#endif

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
        Serial.printf("%s[RL] theta=%.2f (mu=%.2f) - monitoring%s\n", node_tag_, theta_, fit_mu_,
#if defined(MONITOR_REL_OBS)
                      " [rel-obs]"
#elif defined(MONITOR_COND_OBS)
                      " [cond-obs]"
#else
                      ""
#endif
        );
      }
    }
    // decision-trace fields for the pre-theta path (CAL/FIT steps)
    out.surprise  = last_surprise_;
    out.trained   = trained_step;
    out.lr_frac   = lr_frac_used;
    out.frame     = frame_cm;
    out.d_match   = d_match_now;
    out.sticky    = sticky_held;
    out.matched   = km;
    return;                                     // no alarms while fitting
  }

  // 5) threshold logic.
#ifdef MONITOR_BAND_ERAS
  // Band-gated eras: the score is the one-step prediction error in a
  // stationary-per-regime space (rel levels, or residuals around the
  // matched centroid), so STREAMING-TD detection gates alarms: a matched
  // state is judged against its OWN matured surprise band (scoreTheta,
  // PERCOND), an unknown state against the global fit theta. This catches
  // faults too small to leave a condition's match radius - the case pure
  // prototype matching structurally misses. The 2026-11-19 positional
  // latch is not needed here: its two justifications (era
  // non-stationarity, re-entry flicker) were symptoms of the z-space
  // collapse these modes remove.
  // Bench re-validation required before the paper claims it.
  // GRACE: a freshly named state (band still forming) is quiet by name -
  // the operator's label vouches for it while the first learning pass
  // converges. The grace expires OBJECTIVELY when the band matures (16
  // quiet-as-commissioning samples, see the observeScore gate) and the
  // score gate takes over. Without it the alarm flaps while the score
  // crawls across the global theta during learning (bench stage H,
  // 2026-11-20). It is a bootstrap, not the 2026-11-19 permanent latch:
  // it cannot outlive the band's maturity.
  // the ACTIVE gate must reflect a band that matured THIS step (2026-11-21
  // rig: the maturity step still judged against the stale global theta and
  // re-alarmed a state its own band called normal - score 3.67 vs gate
  // 0.57 one line after "band MATURED, gate = 6.235"). The km < 0 case
  // already carries the nearest-condition override from above. Cond era
  // only: the rel paper arm keeps its documented stale-gate behavior.
#ifdef MONITOR_COND_OBS
#if ABL_PERCOND_BAND
  if (km >= 0 && lib_) gate = lib_->scoreTheta(km, theta_);
#endif
#endif
  // Ablation (ABL_PERCOND_BAND=0): no band formation to be in grace for -
  // the fresh-label grace, the re-entry grace and the re-entry
  // recalibration all go with the bands they protect.
#if ABL_PERCOND_BAND
  bool in_grace = (km >= 0 && lib_ && lib_->at(km).s_n < cfg::PERCOND_MIN_N);
#else
  bool in_grace = false;
#endif
#ifdef MONITOR_COND_OBS
  // Re-entry (node_config.h REENTRY_GRACE_N): within the window, the
  // first matched step of a condition whose band is still MATURE
  // recalibrates it - the net's converged floor changed after the
  // regime-entry reset, and the band's outlier gate would block the new
  // floor forever. The reset hands over to the fresh-label grace
  // (s_n < 16). Lazy check: the match may re-acquire a step or two after
  // the regime entry itself.
  // GEOMETRY GUARD (2026-11-22 suite finding): only recalibrate when the
  // position sits INSIDE the condition's own positional band - the state's
  // geometry is its old self and only the prediction floor moved. A fault
  // that coincides with a transition moves the geometry (the sim's
  // in-radius fault: r_agg ~25 vs gate_r ~6) and must NOT be absorbed:
  // without this guard the recalibrated band matured around the fault's
  // scores and the cond-era in-radius guarantee died.
#if ABL_PERCOND_BAND
  if (km >= 0 && reentry_grace_ > 0 &&
      lib_->at(km).s_n >= cfg::PERCOND_MIN_N) {
    float pt = lib_->posTheta(km);
    if (pt <= 0.0f || ragg <= pt) {
      lib_->resetScoreBand(km);
      Serial.printf("%s[WHY] score band of '%s' recalibrated on re-entry (%d"
                    " steps): the net's converged floor changed after the"
                    " regime-entry reset - the band re-forms under grace\n", node_tag_,
                    lib_->at(km).id, cfg::REENTRY_GRACE_N);
    }
  }
  if (km >= 0 && reentry_grace_ > 0) in_grace = true;
#endif  // ABL_PERCOND_BAND
#endif
#ifdef MONITOR_COND_OBS
  // Sustained-floor recalibration (node_config.h FLOOR_EXCESS_N, 2026-11-22
  // real-data tour): the re-entry path above needs a regime ENTRY, but a
  // band can be left BELOW the state's floor by drift or a re-adaptation
  // reset INSIDE one visit. Matched + settled + score above the state's
  // own gate with QUIET GEOMETRY (r_agg under 3x its positional band): the
  // floor moved, the geometry did not - restart the score band, re-arm
  // the grace, and void the stale score evidence (it was judged against
  // the band being replaced). 2026-11-22f: the old SUBSIDE requirement
  // (r_agg decaying 10% from the counter's start) lost a race against the
  // alarm on rag noise - the g100's speed_1 floor rose to 0.27 over a band
  // stuck at 0.012 and every churn visit alarm-edged first (24/cycle). The
  // subside test was also the wrong guard: what separates a floor move
  // from an in-radius fault is the POSITIONAL evidence (a fault breaches
  // gate_r and is caught by the pos_acc_ path, which this re-anchor does
  // NOT void) - sustained score excess over quiet geometry IS the floor
  // move. The void lands AFTER this step's charge (below) so the same
  // step's evidence cannot re-cap the accumulator under the new band.
  // Ablation (ABL_FLOOR_REANCHOR=0): no re-anchor and no floor-move
  // yield - a band left below the state's floor alarm-edges every visit.
#if ABL_FLOOR_REANCHOR
  if (km < 0 || !settled_ || in_grace || sticky_held ||
      km != floor_excess_km_ ||
      score_ <= gate ||
      (gate_r > 0.0f && ragg >= 3.0f * gate_r) ||
      (km == prev_ragg_km_ &&
       ragg - prev_ragg_ > cfg::RAGG_JUMP_MAX)) {
    floor_excess_n_ = 0;
  }
  if (floor_excess_n_ == 0) {
    // (re)start the sustained-excess counter
    if (km >= 0 && settled_ && !in_grace && !sticky_held &&
        score_ > gate &&
        !(gate_r > 0.0f && ragg >= 3.0f * gate_r) &&
        !(km == prev_ragg_km_ && ragg - prev_ragg_ > cfg::RAGG_JUMP_MAX)) {
      floor_excess_n_ = 1;
      floor_excess_km_ = km;
    }
  } else if (km == floor_excess_km_ &&
             ++floor_excess_n_ >= cfg::FLOOR_EXCESS_N) {
    lib_->reseedScoreBand(km, score_);   // mature re-anchor: no grace, no
                                         // coverage gap, no vouch silence
    score_reanchored = true;             // stale score evidence is void
    floor_excess_n_ = 0;
    Serial.printf("%s[WHY] score band of '%s' re-anchored on sustained"
                  " floor excess (%d settled matched steps above its own"
                  " gate, geometry quiet): the machine's floor drifted -"
                  " the band follows it\n", node_tag_,
                  lib_->at(km).id, cfg::FLOOR_EXCESS_N);
  }
#endif  // ABL_FLOOR_REANCHOR
  prev_ragg_ = ragg;
  prev_ragg_km_ = km;
#ifdef MONITOR_COND_OBS
  // Positional-excursion re-anchor (node_config.h POS_EX_*, 2026-11-22b
  // stage-8 finding): the positional band freezes on its first 16 samples
  // (observePos's outlier gate - correct against a fault self-absorbing,
  // wrong against the collect block's internal warm-up, whose tail rows
  // sit up to ~30x a gate formed on the block's cool head rows). The
  // frozen band re-alarmed its OWN rows every block cycle and the
  // accumulator latch could not clear between cycles. The discriminator
  // the suite's in-radius fault cannot fake is the SUBSIDE: a fault is
  // static and re-charges the accumulator every step until named, while
  // the block's own wander falls back inside the gate while the state is
  // still matched. So: an excursion of POS_EX_MIN_N+ settled matched
  // breach steps that then subsides for POS_EX_SUBSIDE_N consecutive
  // matched steps re-anchors the band MATURE at the excursion's peak
  // (reseedPosBand: Welford seed + wander floor) and VOIDS the stale
  // evidence - the alarm the stale band opened clears through the normal
  // hysteresis the next step. A fault never subsides -> never re-anchors
  // -> alarms unhindered (the 2026-11-22 stage-7 contract). A fault-like
  // ARRIVAL (ragg jump over RAGG_JUMP_MAX in one step) disarms the
  // tracker until the breach ends: repaired-fault geometry must not
  // loosen the band.
  if (km < 0 || !settled_ || in_grace || sticky_held || gate_r <= 0.0f) {
    pos_ex_n_ = 0;                             // the excursion's judge left:
    pos_ex_max_ = 0.0f;                        // match, settle, grace, hold
    pos_sub_n_ = 0;                            // or an active gate
  } else if (ragg > gate_r) {                  // breach
    pos_sub_n_ = 0;
    if (km == prev_ragg_km_ &&
        ragg - prev_ragg_ > cfg::RAGG_JUMP_MAX) {
      pos_ex_n_ = -1;                          // geometry moved like a fault:
      pos_ex_max_ = 0.0f;                      // disarm until it ends
    } else if (pos_ex_n_ >= 0) {
      pos_ex_max_ = (pos_ex_n_ == 0)
                        ? ragg : fmaxf(pos_ex_max_, ragg);
      pos_ex_n_++;
    }
  } else if (pos_ex_n_ != 0) {                 // inside the gate
    if (pos_ex_n_ < 0) {                       // re-arm after a disarming jump
      pos_ex_n_ = 0;
      pos_ex_max_ = 0.0f;
    } else if (++pos_sub_n_ >= cfg::POS_EX_SUBSIDE_N) {
      if (pos_ex_n_ >= cfg::POS_EX_MIN_N && pos_ex_max_ > gate_r) {
        lib_->reseedPosBand(km, pos_ex_max_);
        alarm_acc_ = 0.0f;                     // stale-band evidence is void
        pos_acc_ = 0.0f;
        Serial.printf("%s[WHY] positional band of '%s' re-anchored: an"
                      " excursion peaked at r_agg %.2f (%.1fx its gate)"
                      " and subsided while the state stayed matched -"
                      " that is the state's own wander; the gate covers"
                      " it now (floor %.2f) and the stale evidence is"
                      " void\n", node_tag_,
                      lib_->at(km).id, (double)pos_ex_max_,
                      (double)(pos_ex_max_ / gate_r),
                      (double)lib_->posTheta(km));
      }
      pos_ex_n_ = 0;                           // the episode is judged either
      pos_ex_max_ = 0.0f;                      // way; the next one starts fresh
    }
  }
#endif
#endif
#ifdef MONITOR_COND_OBS
  // Grace vouches for the freshly named state's NEIGHBORHOOD (2026-11-21
  // rig replay): a 1-2 step flicker out of the tight batch radius (pf sat
  // d=4.02 on a 15-sample batch) is one saturated evidence step (ev ~54
  // vs ACC_ALARM 5) - an instant re-alarm of the state the operator just
  // named. The label's grace therefore extends to unknown settled steps
  // near the in-grace condition (within YARDSTICK_FAR). Same objective
  // expiry as the matched-path grace: the band's maturity.
  if (!in_grace && km < 0 && lib_ && lib_->size() > 0) {
    float nd = -1.0f;
    int nc = lib_->nearest(last_raw_, space(), &nd);
    if (nc >= 0 && nd <= cfg::YARDSTICK_FAR &&
        lib_->at(nc).s_n < cfg::PERCOND_MIN_N)
      in_grace = true;                     // vouched neighborhood
  }
#endif
  in_grace_now = in_grace;
  if (in_grace && !prev_grace_ && km >= 0) {
    if (lib_->at(km).s_n < cfg::PERCOND_MIN_N)
      Serial.printf("%s[WHY] grace ON for '%s' (score band forming, s_n=%lu/%d):"
                    " gate inactive, accumulator flushes 2x/step\n", node_tag_,
                    lib_->at(km).id, (unsigned long)lib_->at(km).s_n,
                    cfg::PERCOND_MIN_N);
    else
      Serial.printf("%s[WHY] re-entry grace ON for '%s' (%d steps): the matured"
                    " band gate is not representative while the net"
                    " re-adapts - gate inactive, accumulator flushes 2x/step\n", node_tag_,
                    lib_->at(km).id, cfg::REENTRY_GRACE_N);
  }
  prev_grace_ = in_grace;
  // EVIDENCE ACCUMULATOR (2026-11-21, replaces the m-of-N Schmitt in the
  // band eras): each settled step contributes how far it sits past the
  // active alarm edge, normalized by that edge's own gate - scale-free
  // across conditions. The accumulator LEAKS every step and a quiet step
  // never resets it (user requirement: normal evidence does not wipe
  // accumulated badness). One marginal breach cannot fire; weak breaches
  // accumulate; one strong one almost suffices. Marginal drift breaches
  // (the stale-band re-entry false alarm the 5-consecutive rule produced,
  // stage H 2026-11-21) integrate sub-threshold, while the in-radius
  // fault crosses ACC_ALARM within ~4 settled steps.
  alarm_acc_ *= 1.0f - cfg::ACC_DECAY;
  pos_acc_   *= 1.0f - cfg::ACC_DECAY;
  unknown_run_ = (settled_ && km < 0 && !sticky_held) ? unknown_run_ + 1 : 0;
  // sticky_held freezes the unknown run (2026-11-22, repeated asks): the
  // hold means the node has IDENTIFIED the state - a known condition
  // within MATCH_Q - so the rows are not unknown and no ask batch accrues.
  // Before this, the label's frame switch + the block's own outlier rows
  // flickered the QUIET_Q match out once per 15-row loop and the episode
  // churned open/cleared, re-asking a labeled state. A genuinely new state
  // held by a neighbor still asks at the tenure release.
  // evidence gate for UNKNOWN steps: a 1-row transition ramp sits between
  // two states, settles (sub-threshold rel motion), and would charge the
  // nearest band 100x in one step - a mid-tour false alarm (run-120222
  // @604). From the 2nd consecutive settled unknown step on, the state is
  // PERSISTENTLY unmatched - that is the fault's signature, charge it.
  bool unknown_persists
#if ABL_UNKNOWN_PERSIST
      = unknown_run_ >= 2;
#else
      = true;   // ablation: the FIRST settled unknown step charges - the
                // 1-row transition ramp charges the nearest band ~100x in
                // one step (the run-120222 @604 mid-tour FA class)
#endif
  bool charge_unknown = km >= 0 || unknown_persists;
  if (settled_ && in_grace) {
    // Positional evidence is grace-INDEPENDENT (2026-11-22d, the bench
    // phase_loss finding): the grace vouches for the PREDICTOR - the
    // net's floor legitimately moves after a label or a regime-entry
    // reset - never for a static geometry breach. A state's own rows sit
    // inside the band they formed, so only a foreign static offset
    // charges here (run-110857: the phase_loss rows sat ~45x speed_2's
    // band while the stuck-band grace pinned acc at 0.00 through a 570x
    // breach). The machine ramps in <2 s: detection latency past the
    // settle gate was pure grace artifact. The score term still flushes:
    // the predictor IS unreliable during grace (the 2026-11-21 revisit
    // alarm, the fresh-label decay tail). An immature band (r_n <
    // PERCOND_MIN_N) has no gate (posTheta -1): nothing charges, the
    // fresh-label flicker protection is intact by construction.
    if (gate_r > 0.0f && charge_unknown) {
      float evp = ragg / gate_r - 1.0f;
      if (evp > 0.0f) {
        alarm_acc_ += evp;
        pos_acc_   += evp;
        if (evp > ev_used) ev_used = evp;
      }
    }
    // The operator's label vouches for the state: pre-label tripwire
    // evidence flushes fast. The 2026-11-20 grace cleared immediately; a
    // plain leak keeps the ask's alarm open ~60 steps past the label
    // (acc is capped at 20, the leak needs ln(20)/ACC_DECAY steps) -
    // measured at the 60-sample budget, stage H 2026-11-21.
    alarm_acc_ *= 0.5f;
  } else if (settled_ && charge_unknown) {
    float ev = fmaxf(score_ / gate,
                     gate_r > 0.0f ? ragg / gate_r : 0.0f) - 1.0f;
    ev_used = ev;
    if (ev > 0.0f) alarm_acc_ += ev;
    if (gate_r > 0.0f) {
      float rr = ragg / gate_r - 1.0f;
      if (rr > 0.0f) pos_acc_ += rr;
    }
  }
  if (alarm_acc_ > cfg::ACC_CAP) alarm_acc_ = cfg::ACC_CAP;
  if (pos_acc_ > cfg::ACC_CAP) pos_acc_ = cfg::ACC_CAP;
  // the pos fire needs the breach STANDING (2026-11-22f) for the last
  // POS_FIRE_STAND_N settled steps (2026-11-22, net-drift settle gate):
  // the collect blocks' warm-up tail rows breach their band for 1-3
  // consecutive rows and subside when the block loops - the old gate
  // flapped unsettled on exactly those rows and suppressed the charge;
  // the net-drift gate settles through them, and the one-step standing
  // rule alarm-edged every churn visit. A real fault stands indefinitely
  // (latency +POS_FIRE_STAND_N-1 steps); the warm-up excursion does not,
  // and the subside-proven re-anchor owns it instead.
  pos_breach_n_ = (settled_ && gate_r > 0.0f && ragg > gate_r)
                      ? pos_breach_n_ + 1 : 0;
  score_breach_n_ = (settled_ && !in_grace && score_ > gate)
                        ? score_breach_n_ + 1 : 0;
  if (score_reanchored) alarm_acc_ = 0.0f;  // the void must land AFTER this
                                            // step's charge: the step's own
                                            // evidence was judged against the
                                            // replaced band and may not re-cap
                                            // the accumulator under the new one
  if (score_reanchored) score_breach_n_ = 0;
  // The fire check respects the grace: evidence accumulated while the
  // match flickered out (unknown-state steps) must not open an alarm on
  // the step the node re-identifies itself - that step is in grace and
  // flushing exactly this evidence (2026-11-22 real-data tour: the pf
  // radius flicker re-alarmed 'speed_1' at 5.5x its fallback gate the
  // moment it re-matched, then the recalibration cycle repeated). If the
  // excess is real, the band gate takes over when the grace ends and the
  // accumulator is still above the edge - the alarm opens then.
  // FLOOR-MOVE YIELD (2026-11-22g, replaces the 2026-11-22f 3-step
  // counter-age yield): on the ramped churn fixture the score band
  // re-forms on the visit's first low-score samples (outlier-gated) while
  // the live floor climbs to ~19x above it mid-visit, and a single noisy
  // ragg step (> RAGG_JUMP_MAX, landing INSIDE 3x the positional band)
  // kept resetting the floor-excess counter - the re-anchor never reached
  // its 2 consecutive steps, the 3-step yield died with the counter, and
  // the score term alarm-edged every visit of a KNOWN state (asks stayed
  // 0: the operator contract held, the local alarm flapped). The durable
  // discriminator is the GEOMETRY, not the count: sustained score excess
  // over QUIET geometry (ragg under 3x the matched band) IS the floor
  // move the re-anchor owns - the score arm yields for as long as the
  // geometry stays quiet, single-step rag excursions included. A fault
  // arrival is a step change that LANDS foreign (the suite's in-radius
  // fault sits at ~4x its band, r_agg 25 vs gate_r 6): the score arm
  // proceeds there, and the pos arm charges independently either way, so
  // a persistent quiet-but-breachy state still alarms through pos_acc_
  // within a few steps. No unbounded hole.
  // Ablation (ABL_FLOOR_REANCHOR=0): no re-anchor owns the floor move, so
  // the score arm never yields either.
#if ABL_FLOOR_REANCHOR
  bool floor_move_case =
      km >= 0 && gate_r > 0.0f && ragg < 3.0f * gate_r;
#else
  bool floor_move_case = false;
#endif
  // BOTH arms under the !alarmed_ && settled_ edge guard (2026-11-22f:
  // the pos arm sat outside it - && binds tighter than || - and re-fired
  // alarm_open every step while the breach stood, superseding the ask
  // episode each 2 s so no batch ever matured). The pos arm stays
  // independent of in_grace/floor_move_case by design: positional
  // evidence is grace-INDEPENDENT and must fire through a floor move.
#if ABL_ACCUMULATOR
  if (!alarmed_ && settled_ &&
      ((alarm_acc_ >= cfg::ACC_ALARM && !in_grace && !floor_move_case &&
        (score_breach_n_ >= cfg::SCORE_FIRE_STAND_N ||
         pos_breach_n_ >= cfg::POS_FIRE_STAND_N)) ||
       (pos_acc_ >= cfg::ACC_ALARM &&
        pos_breach_n_ >= cfg::POS_FIRE_STAND_N))) {
    alarmed_ = true;
    out.alarm_open = true;
    Serial.printf("%s[RL] ALARM score=%.2f gate=%.2f acc=%.2f\n", node_tag_,
                  score_, gate, fmaxf(alarm_acc_, pos_acc_));
    // [WHY] name the evidence term that DROVE the trigger: during grace
    // only the positional term charges (the score term flushes), so the
    // max() display lied ("score 149x" while the score was flushed) and
    // sent the 2026-11-22f investigation down a dead end
    float sr = gate > 0 ? score_ / gate : 0.0f;
    float rr = gate_r > 0 ? ragg / gate_r : 0.0f;
    bool score_drove = (sr >= rr) && !in_grace_now;
    float acc_shown = fmaxf(alarm_acc_, pos_acc_);
    if (km >= 0)
      Serial.printf("%s[WHY] ALARM '%s': acc %.2f reached %.0f - driver: %s"
                    " %.2f / gate %.2f = %.1fx over its own band%s\n", node_tag_,
                    lib_->at(km).id, (double)acc_shown,
                    (double)cfg::ACC_ALARM,
                    score_drove ? "score" : "r_agg",
                    (double)(score_drove ? score_ : ragg),
                    (double)(score_drove ? gate : gate_r),
                    (double)fmaxf(score_drove ? sr : rr, 1.0f),
                    in_grace_now ? " (grace: only r_agg charges)" : "");
    else {
      float nd = -1.0f;
      int nc = lib_ ? lib_->nearest(last_raw_, space(), &nd) : -1;
      Serial.printf("%s[WHY] ALARM unknown state: acc %.2f reached %.0f -"
                    " %s %.2f / %s %.2f = %.1fx (nothing within MATCH_Q"
                    " %.1f: nearest '%s' d=%.2f%s)\n", node_tag_,
                    (double)acc_shown, (double)cfg::ACC_ALARM,
                    score_drove ? "score" : "r_agg",
                    (double)(score_drove ? score_ : ragg),
                    score_drove ? "gate" : "gate_r",
                    (double)(score_drove ? gate : gate_r),
                    (double)fmaxf(score_drove ? sr : rr, 1.0f),
                    (double)cfg::MATCH_Q,
                    (nc >= 0) ? lib_->at(nc).id : "none",
                    (double)nd,
                    (nc >= 0 && gate != theta_)
                        ? ", judged against its own matured band"
                        : (nc >= 0 ? ", judged against the fit theta"
                                   : ", library empty"));
    }
  } else if (alarmed_ && alarm_acc_ <= cfg::ACC_CLEAR) {
    alarmed_ = false;
    out.alarm_clear = true;
    Serial.printf("%s[RL] clear  score=%.2f gate=%.2f acc=%.2f\n", node_tag_,
                  score_, gate, alarm_acc_);
    Serial.printf("%s[WHY] clear: acc %.2f <= %.0f (%s)\n", node_tag_,
                  (double)alarm_acc_, (double)cfg::ACC_CLEAR,
                  in_grace_now ? "grace flush after the label"
                               : "leak + quiet evidence");
  }
#else
  // Ablation (ABL_ACCUMULATOR=0): the pre-accumulator m-of-N Schmitt, the
  // z-era rule the accumulator replaced. Fire = ALARM_HI_N consecutive
  // settled breaches on either arm (no scale-free integration, no
  // standing-proof margin beyond the count itself, so a breach whose
  // evidence leaks in slowly - the marginal drift class - must still
  // breach every one of the N steps). Clear = CLEAR_LO_N consecutive
  // quiet settled steps (both breach counters back to 0), the z-era
  // hysteresis. The accumulator still charges above (trace/StepOut), it
  // just no longer decides.
  quiet_n_ = (settled_ && score_breach_n_ == 0 && pos_breach_n_ == 0)
                 ? quiet_n_ + 1 : 0;
  if (!alarmed_ && settled_ &&
      ((score_breach_n_ >= cfg::ALARM_HI_N && !in_grace &&
        !floor_move_case) ||
       pos_breach_n_ >= cfg::ALARM_HI_N)) {
    alarmed_ = true;
    out.alarm_open = true;
    Serial.printf("%s[RL] ALARM score=%.2f gate=%.2f (schmitt"
                  " score %d/%d, pos %d/%d)\n", node_tag_,
                  score_, gate, score_breach_n_, cfg::ALARM_HI_N,
                  pos_breach_n_, cfg::ALARM_HI_N);
  } else if (alarmed_ && quiet_n_ >= cfg::CLEAR_LO_N) {
    alarmed_ = false;
    out.alarm_clear = true;
    Serial.printf("%s[RL] clear  score=%.2f gate=%.2f (schmitt quiet"
                  " %d/%d)\n", node_tag_, score_, gate, quiet_n_,
                  cfg::CLEAR_LO_N);
  }
#endif  // ABL_ACCUMULATOR
#else
  // z era (legacy). ALARM MEANS "I DON'T KNOW THIS STATE". The positional
  // SCHMITT latch (in/out at MATCH_Q) covers any KNOWN condition, normal
  // OR fault (2026-11-19): the operator named it, so a recurrence is not
  // news - the condition name is surfaced while quiet and that is the
  // evidence. The latch radius is the HOLD radius (MATCH_Q), not QUIET_Q:
  // a revisit whose geometry re-adapts between the two (2026-11-22 churn:
  // the first revisit sat at d=3.3 with QUIET_Q 3.0 / MATCH_Q 4.0 and the
  // bare streak alarmed exactly once) is a known state re-adapting, not
  // news - the same semantic the band eras' sticky hold carries. The ASK
  // path still keys on QUIET_Q, so a genuinely new state inside the hold
  // radius still stores a batch and asks.
  // Labeled faults that re-alarm on every visit
  // were the operator-facing noise of the 2026-11-19 bench report ("after
  // some back and forth everything is wrong"). Unmatched states gate on
  // the global fit theta: unknown regimes keep alarming until named
  // (2026-11-15 policy), drift out of a known state re-alarms.
  // Per-condition SCORE bands as alarm thresholds were tried and rejected
  // the same day: the score at a state is NOT stationary across training
  // eras (the stopped score rose 1.2 -> ~6 after the speed states were
  // learned), so a band either false-alarms on every era shift or - with
  // an outlier gate - freezes and alarms forever. The band survives where
  // it is sound: converged-state detection for the LR floor (step 3) and
  // per-condition diagnostics in the library dump.
  if (!settled_) {
    lo_ = hi_ = 0;      // mid-transition: no alarm credit, no clear credit
  } else if (reentry_grace_ > 0) {
    // regime-entry grace (all eras): the net re-adapts after the
    // optimizer reset and its surprise decays from the old regime's
    // floor - hold the streak until it does (2026-11-22 z-era churn)
    hi_ = 0; lo_ = 0;
  } else {
    int mq = lib_ ? lib_->match(last_raw_, space(), cfg::MATCH_Q) : -1;
    if (quiet_latch_ && mq < 0)
      quiet_latch_ = false;             // left the hold radius: re-earn
    if (!quiet_latch_ && mq >= 0)
      quiet_latch_ = true;              // any known condition in hold radius
    if (quiet_latch_) {
      lo_ = cfg::CLEAR_LO_N;  hi_ = 0;    // known: quiet
    } else if (score_ > theta_) {
      hi_++; lo_ = 0;                     // unknown and surprising: anomaly
    } else {
      hi_ = 0; lo_++;                     // unknown but quiet (tripwire's turf)
    }
  }
#endif

  out.matched = km;
  out.pos_ref = pos_ref_km;
#ifdef MONITOR_BAND_ERAS
  out.gate = in_grace ? -1.0f : gate;   // -1 = quiet by name (grace)
#else
  out.gate = quiet_latch_ ? -1.0f : theta_;   // -1 = quiet by position (latch)
#endif
  out.ragg   = ragg;
  out.gate_r = gate_r;
  // decision-trace fields (console 'd'; MQTT schema untouched)
  out.surprise  = last_surprise_;
  out.ev        = ev_used;
  out.in_grace  = in_grace_now;
  out.sticky    = sticky_held;
  out.trained   = trained_step;
  out.lr_frac   = lr_frac_used;
  out.d_match   = d_match_now;
  out.frame     = frame_cm;
#ifdef MONITOR_BAND_ERAS
  out.acc = alarm_acc_;
#else
  if (!alarmed_ && hi_ >= cfg::ALARM_HI_N) {
    alarmed_ = true;
    out.alarm_open = true;
    Serial.printf("%s[RL] ALARM score=%.2f theta=%.2f\n", node_tag_, score_, theta_);
  } else if (alarmed_ && lo_ >= cfg::CLEAR_LO_N) {
    alarmed_ = false;
    out.alarm_clear = true;
    Serial.printf("%s[RL] clear  score=%.2f theta=%.2f\n", node_tag_, score_, theta_);
  }
#endif
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
    // MATCH_Q, not QUIET_Q: the sticky match holds a known condition in
    // the QUIET_Q..MATCH_Q band, and the confidence should read that hold
    // (the old QUIET_Q read 0 exactly while the state was still held)
    float d = lib_->matchDist(last_raw_, fs_, cfg::MATCH_Q, &idx);
    if (idx >= 0)
      return fminf(1.0f, fmaxf(0.0f, 1.0f - d / cfg::MATCH_Q));
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
