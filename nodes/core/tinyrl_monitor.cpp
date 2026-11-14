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

void TinyRLConditionMonitor::begin(const FeatureProfile& prof) {
  prof_ = &prof;
  nf_ = prof.n_features;
  fs_.begin(nf_);
  buildNet();
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
  obsnorm_.reset();
  score_ = last_surprise_ = 1.0f;
  step_ = fit_n_ = 0;
  fit_mu_ = fit_m2_ = theta_ = 0;
  hi_ = lo_ = 0;
  learning_ = theta_ready_ = alarmed_ = false;
  has_prev_ = false;
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
    out(0, f) = (x(0, f) - nm_mean_(0, f)) / sd;
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

  // 2) TinyRL observation: running z-scores while fitting, frozen after
  ag::Matrix x(1, nf_);
  memcpy(x.data(), raw, nf_ * sizeof(float));
  ag::Matrix xn = theta_ready_ ? frozenNorm(x) : welfordZScore(*obsnorm_, x);

  // 3) one-step prediction + bounded update (StreamQ update() pattern,
  //    minus the action: reward = -surprise, maximize it)
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
    opt_.step(-surprise, /*reset=*/false);      // delta = -surprise < 0
    reward.clear_graph();                       // -> descent on surprise,
    last_surprise_ = surprise;                  // bounded by kappa rule
  } else {
    last_surprise_ = 1.0f;
  }
  prev_xn_ = xn;
  has_prev_ = true;

  // 4) score EMA + phase machine
  score_ += cfg::SCORE_EMA * (last_surprise_ - score_);
  step_++;

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
        Serial.printf("[RL] theta=%.2f (mu=%.2f) - monitoring\n", theta_, fit_mu_);
      }
    }
    return;                                     // no alarms while fitting
  }

  // 5) threshold logic (MVP semantics, one repair): the 0.7..1.0 theta
  // band grants no alarm credit (anti-flap near theta), but it DOES count
  // as quiet. With theta = mu + 4 sigma and mu dominating, 0.7*theta sits
  // inside the quiet score distribution, so requiring 60 consecutive
  // steps UNDER 0.7*theta made a self-clear structurally impossible -
  // found by the interactive bench (2026-11-13): the score spends ~half
  // its quiet steps in the band and the alarm latched forever.
  if (score_ > theta_)                 { hi_++; lo_ = 0; }
  else if (score_ > 0.7f * theta_)     { hi_ = 0; lo_++; }
  else                                 { lo_++; hi_ = 0; }

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
  score_ = s.score; fit_mu_ = s.fit_mu; fit_m2_ = s.fit_m2; theta_ = s.theta;
  step_ = s.step;
  hi_ = s.hi; lo_ = s.lo;
  theta_ready_ = s.theta_ready != 0;
  alarmed_ = s.alarm != 0;
  has_prev_ = false;

  if (theta_ready_) {
    // exact resume: frozen ranges + frozen normalizer are sufficient
    fs_.begin(nf_);
    for (int f = 0; f < nf_; ++f) fs_.setRange(f, s.fs_lo[f], s.fs_hi[f]);
    fs_.forceArmed(s.fs_armed != 0, s.fs_steps);
    for (int f = 0; f < nf_; ++f) {
      nm_mean_(0, f) = s.nm_mean[f];
      nm_var_(0, f)  = s.nm_var[f];
    }
    obsnorm_.reset();
    learning_ = false;
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
