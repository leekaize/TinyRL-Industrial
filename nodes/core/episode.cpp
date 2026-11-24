// =====================================================================
// episode.cpp -- generic episode lifecycle (open/feed/close/label).
// =====================================================================
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "episode.h"

void EpisodeManager::begin(const FeatureProfile* prof,
                           ConditionLibrary* lib,
                           const char* dev_id, EventFn fn) {
  prof_ = prof; lib_ = lib; dev_id_ = dev_id; fn_ = std::move(fn);
  open_ = false; n_ = 0; cond_[0] = 0;
  memset(sum_, 0, sizeof(sum_));
  memset(rmean_, 0, sizeof(rmean_));
  memset(rm2_, 0, sizeof(rm2_));
}

uint32_t EpisodeManager::duration(uint32_t now) const {
  return open_ ? (now - t0_) / 1000 : 0;
}

void EpisodeManager::means(float* out) const {
  for (int f = 0; f < prof_->n_features; ++f)
    out[f] = n_ ? sum_[f] / (float)n_ : 0.0f;
}

void EpisodeManager::relMeans(float* out) const {
  for (int f = 0; f < prof_->n_features; ++f) out[f] = rmean_[f];
}

void EpisodeManager::relStd(float* out) const {
  for (int f = 0; f < prof_->n_features; ++f)
    out[f] = n_ > 1 ? sqrtf(rm2_[f] / (float)(n_ - 1)) : 0.0f;
}

size_t EpisodeManager::summary(char* out, size_t cap, uint32_t now) const {
  size_t o = 0;
  int w = snprintf(out + o, cap - o, "%s:", dev_id_);
  if (w < 0) return 0;
  o += w;
  for (int f = 0; f < prof_->n_features && o < cap; ++f) {
    float m = n_ ? sum_[f] / (float)n_ : 0.0f;
    w = snprintf(out + o, cap - o, "%s %s=%.4g%s",
                 f ? "," : " ", prof_->features[f].name, m,
                 prof_->features[f].unit);
    if (w < 0) break;
    o += w;
  }
  if (open_ && o < cap)
    o += snprintf(out + o, cap - o, " for %lus", (unsigned long)((now - t0_) / 1000));
  return o;
}

void EpisodeManager::open(const float* x, const FeatureSpace& fs, uint32_t now) {
  open_ = true;
  t0_ = now;
  n_ = 1;
  relabelled_ = false;
  for (int f = 0; f < prof_->n_features; ++f) {
    sum_[f] = x[f];
    float r = fs.rel(f, x[f]);
    rmean_[f] = r;
    rm2_[f] = 0.0f;
  }
  int m = lib_ ? lib_->match(x, fs, cfg::QUIET_Q) : -1;
  // pre-name ONLY on a confident match (QUIET_Q): at a loose radius the
  // start-transient centroid swallowed real faults and recorded them
  // under a normal's name with no ask (bench finding, 2026-11-14).
  // Ambiguous openings stay "unknown" and ask - that is the design: the
  // operator (or agent) decides, no node-to-node consultation.
  snprintf(cond_, sizeof(cond_), "%s", m >= 0 ? lib_->at(m).id : "unknown");
  kind_ = (m >= 0) ? lib_->at(m).kind : COND_FAULT;

  char sum[192];
  summary(sum, sizeof(sum), now);
  float mean[cfg::MAX_FEATURES];
  means(mean);
  if (fn_) fn_("open", "alarm", cond_, 0, sum, mean);
  Serial.printf("%s[EP] open cond=%s\n", dev_id_, cond_);
}

void EpisodeManager::feed(const float* x, const FeatureSpace& fs) {
  if (!open_) return;
  n_++;
  for (int f = 0; f < prof_->n_features; ++f) {
    sum_[f] += x[f];
    // Welford in the condition space: rmean is the centroid a label
    // stores, rm2 the spread its matching units come from
    float r = fs.rel(f, x[f]);
    float d = r - rmean_[f];
    rmean_[f] += d / (float)n_;
    rm2_[f] += d * (r - rmean_[f]);
  }
}

void EpisodeManager::close(const char* how, const FeatureSpace& fs, uint32_t now) {
  if (!open_) return;
  float mean[cfg::MAX_FEATURES];
  means(mean);
  uint32_t dur = duration(now);
  char sum[192];
  summary(sum, sizeof(sum), now);

  // Silent centroid refinement on a quiet clear (EMA 0.9/0.1), NORMAL-kind
  // episodes only (bench finding 2026-11-19): a fault episode's mean folds
  // in the recovery tail that precedes the clear, dragging the fault
  // centroid toward the recovered state - the blob that let one learned
  // fault swallow neighbouring states. Fault centroids come from explicit
  // labels; normals may drift-track. RELABELED episodes never refine
  // (2026-11-22 real-data tour): their mean spans the state that moved
  // under them (the transition + the destination's rows) and would drag
  // the relabeled condition's identity across the gap.
  if (strcmp(how, "cleared") == 0 && strcmp(cond_, "unknown") != 0 &&
      kind_ == COND_NORMAL && lib_ && !relabelled_)
    lib_->refine(cond_, mean, fs);              // EMA centroid refinement

  if (fn_) fn_("close", strcmp(how, "cleared") == 0 ? "alarm" : "info",
               cond_, dur, sum, mean);
  Serial.printf("%s[EP] close cond=%s dur=%lus (%s)\n", dev_id_, cond_,
                (unsigned long)dur, how);
  open_ = false;
}

void EpisodeManager::relabel(const char* slug, CondKind kind, uint32_t now) {
  if (!open_) return;
  snprintf(cond_, sizeof(cond_), "%s", slug);
  kind_ = kind;
  relabelled_ = true;            // the mean now spans two states
  char sum[192];
  summary(sum, sizeof(sum), now);
  float mean[cfg::MAX_FEATURES];
  means(mean);
  if (fn_) fn_("label", "info", cond_, duration(now), sum, mean);
}

void EpisodeManager::drop() {
  // factory reset: no summary, no event - the whole world is being wiped
  open_ = false;
  t0_ = 0;
  n_ = 0;
  cond_[0] = 0;
  kind_ = COND_FAULT;
}
