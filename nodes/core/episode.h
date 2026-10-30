// =====================================================================
// episode.h -- alarm episode lifecycle, GENERIC over the feature profile.
//
// open -> feed (while alarmed) -> close("cleared" | "normal").
// Summaries and per-feature means are built from FeatureProfile names and
// units, so the same code serves a VFD node, a vibration node, etc.
// Publishing goes through an injected EventFn (keeps this module free of
// MQTT knowledge).
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
  void feed(const float* x);
  void close(const char* how, const FeatureSpace& fs, uint32_t now);
  void relabel(const char* slug, uint32_t now);   // after Action/feedback

  bool     isOpen() const { return open_; }
  const char* cond() const { return cond_[0] ? cond_ : "unknown"; }
  uint32_t duration(uint32_t now) const;
  void     means(float* out) const;               // raw engineering units
  size_t   summary(char* out, size_t cap, uint32_t now) const;

private:
  const FeatureProfile*  prof_ = nullptr;
  ConditionLibrary*      lib_ = nullptr;
  EventFn fn_;
  const char* dev_id_ = "";

  bool     open_ = false;
  uint32_t t0_ = 0, n_ = 0;
  float    sum_[cfg::MAX_FEATURES];
  char     cond_[24];
};
