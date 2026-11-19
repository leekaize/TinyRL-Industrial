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
  // per-condition score calibration (2026-11-19): Welford of the surprise
  // score observed on settled matched steps of THIS condition. The global
  // theta is fit once at commissioning and goes stale as soon as the net
  // trains on other regimes (bench: vib quiet score ~3 vs theta ~0.3) -
  // quiet then leans entirely on the positional latch, and every match
  // flicker alarms. A condition that knows its own score band answers
  // "is 8.3 surprising HERE?" correctly for every state.
  float    s_mu = 0;                            // score mean
  float    s_m2 = 0;                            // Welford m2
  uint32_t s_n = 0;                             // samples seen
};

class ConditionLibrary {
public:
  void begin(int nf);

  // nearest-centroid match; q scales the per-feature radius
  // (q * u(f), condition's own units or the CAL fallback). -1 = no
  // confident match. MAX-COMPONENT metric; among qualifying conditions
  // the NEAREST wins (smallest normalized distance) - first-inside made
  // the win order depend on label sequence once radii overlap. Runs on
  // the CALLER's rel position - use the monitor's smoothed state estimate
  // (stateRel), never a raw sample.
  int  match(const float* raw, const FeatureSpace& fs,
             float q = cfg::MATCH_Q) const;

  // label a condition from its settled batch: centroid = the batch's rel
  // mean, units = the batch's rel sigma (0-entry keeps the previous
  // units; the auto-registered baseline passes all-zero and keeps the
  // CAL fallback). Creates or reinforces; returns condition index.
  int  label(const char* slug, const float* mean_rel, const float* sd_rel,
             const char* by, CondKind kind = COND_FAULT);

  // per-condition score calibration (2026-11-19): feed the surprise score
  // observed on a settled matched step of condition idx. Outlier-gated:
  // once the band is mature, steps above mu + 3 sigma never teach it (a
  // real anomaly must not widen its own alarm threshold).
  void observeScore(int idx, float score);

  // the condition's own alarm threshold: mu + k sigma of its observed
  // quiet scores once mature (s_n >= cfg::PERCOND_MIN_N), else `fallback`
  // (the global fit theta). k = cfg::PERCOND_KSIG.
  float scoreTheta(int idx, float fallback) const;

  // true when the condition's score band is mature AND `score` sits
  // inside it: the net already predicts this state, so repeated entries
  // need only floor-rate refinement, not full novelty-rate re-learning.
  bool converged(int idx, float score) const;

  // worst normalized distance of the NEAREST qualifying condition (per-
  // feature distance / unit, max over features): 0 = dead centre,
  // q = at the radius edge. *idx = the condition index or -1.
  float matchDist(const float* raw, const FeatureSpace& fs, float q,
                  int* idx) const;

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
