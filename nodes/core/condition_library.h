// =====================================================================
// condition_library.h -- on-device library of learned conditions.
// Nearest-centroid classifier in the FROZEN 0..1 condition space.
// General core: feature count/order comes from the FeatureProfile.
// Persistence: JSONL (one condition per line), human-inspectable.
// =====================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "node_config.h"
#include "feature_space.h"

struct Condition {
  char     id[24];                              // slug, e.g. "belt_slip"
  float    c[cfg::MAX_FEATURES];                // centroid in 0..1 space
  uint32_t count;                               // episodes seen
  uint32_t first_seen, last_seen;               // device millis()
  char     source[12];                          // human | agent | console
};

class ConditionLibrary {
public:
  void begin(int nf);

  // nearest-centroid match in condition space; -1 = no confident match
  int  match(const float* raw, const FeatureSpace& fs) const;

  // label an episode mean: creates or reinforces; returns condition index
  int  label(const char* slug, const float* mean_raw, const FeatureSpace& fs,
             const char* by);

  // silent refinement on episode close (EMA 0.9/0.1), MVP semantics
  void refine(const char* id, const float* mean_raw, const FeatureSpace& fs);

  int  size() const { return n_; }
  const Condition& at(int i) const { return lib_[i]; }
  void clear() { n_ = 0; }

  void list() const;                            // console dump

  bool load(const char* path);
  bool save(const char* path) const;

private:
  int find(const char* slug) const;

  Condition lib_[cfg::LIB_MAX];
  int       n_ = 0;
  int       nf_ = 0;
};
