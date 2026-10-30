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
}

uint32_t EpisodeManager::duration(uint32_t now) const {
  return open_ ? (now - t0_) / 1000 : 0;
}

void EpisodeManager::means(float* out) const {
  for (int f = 0; f < prof_->n_features; ++f)
    out[f] = n_ ? sum_[f] / (float)n_ : 0.0f;
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
  for (int f = 0; f < prof_->n_features; ++f) sum_[f] = x[f];
  int m = lib_ ? lib_->match(x, fs) : -1;
  snprintf(cond_, sizeof(cond_), "%s", m >= 0 ? lib_->at(m).id : "unknown");

  char sum[192];
  summary(sum, sizeof(sum), now);
  float mean[cfg::MAX_FEATURES];
  means(mean);
  if (fn_) fn_("open", "alarm", cond_, 0, sum, mean);
  Serial.printf("[EP] open cond=%s\n", cond_);
}

void EpisodeManager::feed(const float* x) {
  if (!open_) return;
  n_++;
  for (int f = 0; f < prof_->n_features; ++f) sum_[f] += x[f];
}

void EpisodeManager::close(const char* how, const FeatureSpace& fs, uint32_t now) {
  if (!open_) return;
  float mean[cfg::MAX_FEATURES];
  means(mean);
  uint32_t dur = duration(now);
  char sum[192];
  summary(sum, sizeof(sum), now);

  if (strcmp(how, "cleared") == 0 && strcmp(cond_, "unknown") != 0 && lib_)
    lib_->refine(cond_, mean, fs);              // EMA centroid refinement

  if (fn_) fn_("close", strcmp(how, "cleared") == 0 ? "alarm" : "info",
               cond_, dur, sum, mean);
  Serial.printf("[EP] close cond=%s dur=%lus (%s)\n", cond_,
                (unsigned long)dur, how);
  open_ = false;
}

void EpisodeManager::relabel(const char* slug, uint32_t now) {
  if (!open_) return;
  snprintf(cond_, sizeof(cond_), "%s", slug);
  char sum[192];
  summary(sum, sizeof(sum), now);
  float mean[cfg::MAX_FEATURES];
  means(mean);
  if (fn_) fn_("label", "info", cond_, duration(now), sum, mean);
}
