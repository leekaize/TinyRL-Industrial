// =====================================================================
// condition_library.h -- on-device library of learned conditions.
// Nearest-centroid classifier in the FROZEN anchored-ratio condition
// space (feature_space.h). General core: feature count/order comes from
// the FeatureProfile. Persistence: JSONL, human-inspectable.
//
// Each condition carries its OWN matching units u[f] (decades of one
// sigma, measured on the labelled batch's rel spread; 2026-11-15).
// The radius around a condition must reflect THAT condition's noise:
// CAL-fallback units are derived from the stopped machine, which made
// the drive's radii ~20 decades (blind to every running state) and the
// vib's ~0.15 decades (narrower than its own noise). u[f] == 0 falls
// back to fs.unit(f) - the auto-registered baseline has no batch yet.
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
  float    c[cfg::MAX_FEATURES];                // centroid, rel decades
  float    u[cfg::MAX_FEATURES];                // per-condition units (0 = CAL fallback)
  uint32_t count;                               // episodes seen
  uint32_t first_seen, last_seen;               // device millis()
  char     source[12];                          // human | agent | console
  CondKind kind = COND_FAULT;                   // normal | fault
};

class ConditionLibrary {
public:
  void begin(int nf);

  // nearest-centroid match; q scales the per-feature radius
  // (q * u(f), condition's own units or the CAL fallback). -1 = no
  // confident match. MAX-COMPONENT metric + NEAREST wins among
  // qualifying conditions. Runs on the CALLER's rel position - use the
  // monitor's smoothed state estimate (stateRel), never a raw sample.
  int  match(const float* raw, const FeatureSpace& fs,
             float q = cfg::MATCH_Q) const;

  // label a condition from its settled batch: centroid = the batch's rel
  // mean, units = the batch's rel sigma (0-entry keeps the previous
  // units; the auto-registered baseline passes all-zero and keeps the
  // CAL fallback). Creates or reinforces; returns condition index.
  int  label(const char* slug, const float* mean_rel, const float* sd_rel,
             const char* by, CondKind kind = COND_FAULT);

  // silent refinement on episode close (EMA 0.9/0.1), MVP semantics
  void refine(const char* id, const float* mean_raw, const FeatureSpace& fs);

  int  size() const { return n_; }
  bool has(const char* slug) const { return find(slug) >= 0; }
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
