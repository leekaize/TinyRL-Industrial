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

// Kind of a learned condition. Zero-init must stay FAULT (conservative:
// memset'd storage and old save files without a kind field never
// auto-accept into the alarm baseline; only an explicitly labelled normal
// condition does).
enum CondKind : uint8_t { COND_FAULT = 0, COND_NORMAL = 1 };

struct Condition {
  char     id[24];                              // slug, e.g. "belt_slip"
  float    c[cfg::MAX_FEATURES];                // centroid in 0..1 space
  uint32_t count;                               // episodes seen
  uint32_t first_seen, last_seen;               // device millis()
  char     source[12];                          // human | agent | console
  CondKind kind = COND_FAULT;                   // normal | fault
};

class ConditionLibrary {
public:
  void begin(int nf);

  // nearest-centroid match; q scales the per-feature radius (q * unit(f),
  // see node_config.h). -1 = no confident match. MAX-COMPONENT metric: a
  // condition matches only if EVERY feature is within its radius.
  int  match(const float* raw, const FeatureSpace& fs,
             float q = cfg::MATCH_Q) const;

  // label an episode mean: creates or reinforces; returns condition index
  int  label(const char* slug, const float* mean_raw, const FeatureSpace& fs,
             const char* by, CondKind kind = COND_FAULT);

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
