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
  // per-condition POSITIONAL band (2026-11-21, cond-obs era): Welford of
  // r_agg = sum_f ((rel_f - c_f)/u_f)^2 over settled matched steps. The
  // one-step predictor cannot see a STATIC offset (it converges to
  // identity, pred(x_{t-1}) ~= x_t, in any coordinate frame), so a fault
  // that sits inside the match radius but 2.5 sigmas off the centroid on
  // every feature is invisible to the score gate. The aggregate residual
  // sees it: quiet steps sit at ~chi2(nf), the fault at nf*2.5^2.
  float    r_mu = 0;                            // r_agg mean
  float    r_m2 = 0;                            // Welford m2
  uint32_t r_n = 0;                             // samples seen
  // positional-gate FLOOR (2026-11-22b, reseedPosBand): the minimum gate
  // the condition's own demonstrated wander earned - an excursion that
  // breached the gate and then SUBSIDED while the state stayed matched is
  // the state's own wander (a fault never subsides), so the gate never
  // shrinks back below peak x (1 + PERCOND_KSIG x POS_RESEED_SD_FRAC).
  // Without it the Welford stats decay back to the cool rows within one
  // block cycle and the warm tail re-alarms its own rows forever.
  float    r_floor = 0;
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
  // `fs` (cond era) floors the units at a fraction of the CAL sigma: a
  // 15-row batch of a near-constant register measures a spread orders of
  // magnitude finer than the register's true wander, and the condition
  // then rejects its own state's rows (node_config.h UNIT_FLOOR_FRAC).
  int  label(const char* slug, const float* mean_rel, const float* sd_rel,
             const char* by, CondKind kind = COND_FAULT,
             const FeatureSpace* fs = nullptr);

  // per-condition score calibration (2026-11-19): feed the surprise score
  // observed on a settled matched step of condition idx. Outlier-gated:
  // once the band is mature, steps above mu + 3 sigma never teach it (a
  // real anomaly must not widen its own alarm threshold).
  void observeScore(int idx, float score);

  // regime-entry recalibration (2026-11-21, real-data replay): the
  // regime-entry optimizer reset legitimately changes the net's converged
  // error floor, and the outlier gate above then blocks the new floor
  // forever - the pristine band gate sat BELOW the post-excursion floor
  // (gate 0.01 vs floor 0.02 on the collect rows) and the state
  // re-alarmed on every revisit. Restart the Welford: the band re-forms
  // on the post-entry floor under the fresh-label grace (s_n < 16).
  // Steady-state anomalies keep the outlier guard - only the regime-entry
  // moment, where the re-adaptation is provable, may recalibrate.
  void resetScoreBand(int idx);

  // re-anchor the band MATURE at `score` (2026-11-22 real-data tour): the
  // sustained-floor path - the machine's floor drifted inside one visit,
  // the geometry is quiet, the net is already at the new floor. No grace,
  // no coverage gap: the gate jumps to the current floor immediately (a
  // grace window here would flush the positional evidence too and keep
  // the grace-vouch alive, silencing the neighbourhood).
  void reseedScoreBand(int idx, float score);

  // re-anchor the POSITIONAL band MATURE at an excursion's peak `rag_max`
  // (2026-11-22b, node_config.h POS_EX_*): the excursion breached the
  // gate for POS_EX_MIN_N+ matched steps and then subsided while the
  // state stayed matched and settled - it is the state's own wander, not
  // a fault (a fault is static and never subsides). Seeds the Welford at
  // the peak and plants r_floor so the Welford's cool-row decay cannot
  // shrink the gate back under the wander the state already demonstrated.
  void reseedPosBand(int idx, float rag_max);

  // the condition's own alarm threshold: mu + k sigma of its observed
  // quiet scores once mature (s_n >= cfg::PERCOND_MIN_N), else `fallback`
  // (the global fit theta). k = cfg::PERCOND_KSIG.
  float scoreTheta(int idx, float fallback) const;

  // per-condition POSITIONAL alarm threshold (cond-obs era): mu + k sigma
  // of the observed r_agg values once mature (r_n >= cfg::PERCOND_MIN_N),
  // else -1 (gate inactive - grace covers the forming band). Feeding is
  // geometry, not prediction: the residual aggregate does not decay after
  // a label, so no transient gate is needed, only the outlier guard.
  void observePos(int idx, float r_agg);
  float posTheta(int idx) const;

  // true when the condition's score band is mature AND `score` sits
  // inside it: the net already predicts this state, so repeated entries
  // need only floor-rate refinement, not full novelty-rate re-learning.
  bool converged(int idx, float score) const;

  // worst normalized distance of the NEAREST qualifying condition (per-
  // feature distance / unit, max over features): 0 = dead centre,
  // q = at the radius edge. *idx = the condition index or -1.
  float matchDist(const float* raw, const FeatureSpace& fs, float q,
                  int* idx) const;

  // worst normalized distance to ONE condition, no radius qualification
  // (same per-feature metric as match()). Radius-edge stickiness: hold a
  // known condition between QUIET_Q and MATCH_Q instead of flickering to
  // unknown at the edge. -1 when the index is out of range.
  float distTo(int idx, const float* raw, const FeatureSpace& fs) const;

  // which feature carries the worst normalized distance to ONE condition
  // (the [WHY] transition lines name it). -1 when the index is invalid.
  int worstFeature(int idx, const float* raw, const FeatureSpace& fs) const;

  // nearest condition of ALL stored ones, NO radius qualification (the
  // max-component metric, nearest wins). For the per-cond centered-obs
  // era: an unknown state is centered on its closest known regime, so the
  // agent's residual input reads "how far off the nearest regime am I" -
  // >= 3 units by definition of a match failure, far off the trained
  // manifold. *dist = the winner's worst normalized distance. -1 when the
  // library is empty.
  int  nearest(const float* raw, const FeatureSpace& fs, float* dist) const;

  // silent refinement on episode close (EMA 0.9/0.1), MVP semantics
  void refine(const char* id, const float* mean_raw, const FeatureSpace& fs);

  // yardstick adaptation (2026-11-21 rig replay, handoff 7.3): grow each
  // unit toward the condition's own observed steady-state spread on quiet
  // matched steps. Grow-only (a smaller observation never shrinks it); a
  // relabel batch refresh can still tighten it. u=0 (CAL fallback, no
  // batch spread) is never adapted.
  void adaptUnits(int idx, const float* raw, const FeatureSpace& fs,
                  float alpha);

  // centroid tracking for slow regime drift (2026-11-21 rig replay): a
  // normal condition's center follows its own settled positions at the
  // same EMA rate (the motor warm-up walked the labeled centroid 15 units
  // out). Per-feature guarded on u>0: a CAL-fallback feature has no
  // measured spread, so its position must not move on that basis. The
  // caller owns the kind/distance gates (normal neighbors only, within
  // YARDSTICK_FAR, settled steps only) - a fault's offset exceeds the
  // bound in one step, so faults cannot pull a normal centroid.
  void adaptCentroid(int idx, const float* raw, const FeatureSpace& fs,
                     float alpha);

  int  size() const { return n_; }
  bool has(const char* slug) const { return find(slug) >= 0; }
  int  indexOf(const char* slug) const { return find(slug); }  // -1 = absent
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
