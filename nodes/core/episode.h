// =====================================================================
// episode.h -- alarm episode lifecycle, GENERIC over the feature profile.
//
// open -> feed (while alarmed) -> close("cleared" | "normal").
// Summaries and per-feature means are built from FeatureProfile names and
// units, so the same code serves a VFD node, a vibration node, etc.
// Publishing goes through an injected EventFn (keeps this module free of
// MQTT knowledge).
//
// The episode accumulates TWO views of the same samples: raw sums (event
// summaries, same-profile centroid mapping) and a Welford run in the
// CONDITION SPACE (rel mean + rel sigma, 2026-11-15). The rel sigma is
// what a label stores as the condition's own matching units: the
// yardstick must be the noise of the condition it belongs to, not the
// stopped-machine CAL noise (bench: CAL-derived radii were 20 decades on
// the drive's dead features and 0.15 decades on the vib's bands - the
// drive went blind, the vib matched on luck).
// =====================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <functional>
#include "sensor_interface.h"
#include "feature_space.h"
#include "condition_library.h"

class EpisodeManager {
public:
  // ev: "open"|"close"|"label", sev: "alarm"|"info", means = raw units
  using EventFn = std::function<void(const char* ev, const char* sev,
                                     const char* cid, uint32_t duration_s,
                                     const char* summary, const float* means)>;

  void begin(const FeatureProfile* prof, ConditionLibrary* lib,
             const char* dev_id, EventFn fn);

  void open(const float* x, const FeatureSpace& fs, uint32_t now);
  void feed(const float* x, const FeatureSpace& fs);
  void close(const char* how, const FeatureSpace& fs, uint32_t now);
  void relabel(const char* slug, CondKind kind, uint32_t now);

  bool     isOpen() const { return open_; }
  const char* cond() const { return cond_[0] ? cond_ : "unknown"; }
  CondKind kind() const { return kind_; }             // normal | fault
  uint32_t count() const { return n_; }               // samples so far
  uint32_t duration(uint32_t now) const;
  void     means(float* out) const;               // raw engineering units
  void     relMeans(float* out) const;            // condition-space mean
  void     relStd(float* out) const;              // condition-space sigma
  size_t   summary(char* out, size_t cap, uint32_t now) const;

private:
  const FeatureProfile*  prof_ = nullptr;
  ConditionLibrary*      lib_ = nullptr;
  EventFn fn_;
  const char* dev_id_ = "";

  bool     open_ = false;
  uint32_t t0_ = 0, n_ = 0;
  float    sum_[cfg::MAX_FEATURES];       // raw, for summaries
  float    rmean_[cfg::MAX_FEATURES];     // condition-space Welford
  float    rm2_[cfg::MAX_FEATURES];
  char     cond_[24];
  CondKind kind_ = COND_FAULT;   // unknown episodes never auto-accept
};
