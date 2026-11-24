// =====================================================================
// condition_library.cpp -- learned-condition storage + matching.
// Logic preserved from the vfd_streamq MVP; only the feature space and
// feature count were generalized (they used to be file-local constants).
// =====================================================================
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <LittleFS.h>
#include "condition_library.h"
#include "json_mini.h"

void ConditionLibrary::begin(int nf) {
  nf_ = nf;
  n_ = 0;
  memset(lib_, 0, sizeof(lib_));
}

int ConditionLibrary::find(const char* slug) const {
  for (int i = 0; i < n_; ++i)
    if (strcmp(lib_[i].id, slug) == 0) return i;
  return -1;
}

int ConditionLibrary::match(const float* raw, const FeatureSpace& fs,
                            float q) const {
  // MAX-COMPONENT metric in log2-ratio decades with a PER-CONDITION
  // radius: q * u(f), the condition's own measured spread (its labelled
  // batch's rel sigma), falling back to the CAL unit when the condition
  // carries none. The old mean-distance over 0..1 let one discriminative
  // feature be outvoted by the flat ones (bench finding, 2026-11-14);
  // CAL-only units made the radii themselves wrong (2026-11-15).
  // 2026-11-19: among qualifying conditions the NEAREST wins (smallest
  // worst-feature normalized distance) - first-inside made the winner
  // depend on label order once radii overlapped.
  int   best = -1;
  float best_d = 0;
  for (int i = 0; i < n_; ++i) {
    float worst = 0;
    bool  ok = true;
    for (int f = 0; f < nf_; ++f) {
      float unit = lib_[i].u[f] > 0 ? lib_[i].u[f] : fs.unit(f);
      float d = fabsf(fs.rel(f, raw[f]) - lib_[i].c[f]) / unit;
      if (d > q) { ok = false; break; }
      if (d > worst) worst = d;
    }
    if (ok && (best < 0 || worst < best_d)) { best = i; best_d = worst; }
  }
  return best;
}

int ConditionLibrary::label(const char* slug, const float* mean_rel,
                            const float* sd_rel, const char* by,
                            CondKind kind,
                            const FeatureSpace* fs /* CAL-unit floor */) {
  int found = find(slug);
  if (found < 0 && n_ < cfg::LIB_MAX) {
    found = n_++;
    Condition& c = lib_[found];
    memset(&c, 0, sizeof(c));
    snprintf(c.id, sizeof(c.id), "%s", slug);
    snprintf(c.source, sizeof(c.source), "%s", (by && by[0]) ? by : "human");
    c.first_seen = millis();
    c.count = 1;
    c.last_seen = millis();
    c.kind = kind;
    for (int f = 0; f < nf_; ++f) {
      c.c[f] = mean_rel[f];
      c.u[f] = sd_rel ? sd_rel[f] : 0.0f;
      // UNIT FLOOR (cond era, 2026-11-22 real-data tour): a 15-row batch
      // of a near-constant register measures a spread ~1000x finer than
      // the register's true wander (the collect campaign's pf: batch sd
      // 0.0001 decades, block wander 0.046) - the condition's radius then
      // rejects its own state's rows and they re-alarm as "unknown". The
      // batch spread stands, but never below a fraction of the CAL sigma:
      // the CAL window is the machine's observed operating resolution.
      if (fs && sd_rel && sd_rel[f] > 0.0f)
        c.u[f] = fmaxf(c.u[f], cfg::UNIT_FLOOR_FRAC * fs->unit(f));
    }
    Serial.printf("[LIB] learned new condition '%s' (%s)\n", slug,
                  kind == COND_NORMAL ? "normal" : "fault");
  } else if (found >= 0) {
    Condition& c = lib_[found];
    for (int f = 0; f < nf_; ++f) {
      c.c[f] = 0.8f * c.c[f] + 0.2f * mean_rel[f];
      if (sd_rel && sd_rel[f] > 0)          // a batch with real spread
        c.u[f] = c.u[f] > 0                 // refreshes the units; a
              ? 0.8f * c.u[f] + 0.2f * sd_rel[f]   // zero-sd source (the
              : sd_rel[f];                         // baseline) never does
      if (fs && sd_rel && sd_rel[f] > 0.0f) // same floor on refresh
        c.u[f] = fmaxf(c.u[f], cfg::UNIT_FLOOR_FRAC * fs->unit(f));
    }
    c.count++;
    c.last_seen = millis();
    c.kind = kind;                    // operator may re-classify
    if (by && by[0]) snprintf(c.source, sizeof(c.source), "%s", by);
  }
  return found;
}

void ConditionLibrary::refine(const char* id, const float* mean_raw,
                              const FeatureSpace& fs) {
  int i = find(id);
  if (i < 0) return;
  Condition& c = lib_[i];
  for (int f = 0; f < nf_; ++f)
    c.c[f] = 0.9f * c.c[f] + 0.1f * fs.rel(f, mean_raw[f]);
  c.count++;
  c.last_seen = millis();
}

void ConditionLibrary::adaptUnits(int idx, const float* raw,
                                  const FeatureSpace& fs, float alpha) {
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  for (int f = 0; f < nf_; ++f) {
    if (c.u[f] <= 0) continue;               // CAL fallback: no batch spread
                                             // to adapt from
    float obs = fabsf(fs.rel(f, raw[f]) - c.c[f]);
    float grown = (1.0f - alpha) * c.u[f] + alpha * obs;
    // CEILING (node_config.h YARDSTICK_U_CAP): the unit covers the
    // state's own wander, never the distance to a neighbouring state.
    float cal_unit = fs.unit(f);
    if (cal_unit > 0.0f) {
      float cap = cfg::YARDSTICK_U_CAP * cal_unit;
      if (grown > cap) grown = cap;
    }
    if (grown > c.u[f]) c.u[f] = grown;      // grow-only: observed spread
  }
}

void ConditionLibrary::adaptCentroid(int idx, const float* raw,
                                     const FeatureSpace& fs, float alpha) {
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  for (int f = 0; f < nf_; ++f) {
    if (c.u[f] <= 0) continue;               // CAL fallback: position unfounded
    c.c[f] += alpha * (fs.rel(f, raw[f]) - c.c[f]);
  }
}

void ConditionLibrary::observeScore(int idx, float score) {
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  if (c.s_n >= cfg::PERCOND_MIN_N) {
    float sd = sqrtf(c.s_m2 / (float)c.s_n);
    if (score > c.s_mu + 3.0f * sd)
      return;                       // anomaly: never widens its own band
  }
  c.s_n++;
  float d = score - c.s_mu;
  c.s_mu += d / (float)c.s_n;
  c.s_m2 += d * (score - c.s_mu);
}

void ConditionLibrary::resetScoreBand(int idx) {
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  c.s_n = 0;
  c.s_mu = 0;
  c.s_m2 = 0;                 // scoreTheta falls back to the global theta
}                             // until the band re-matures under grace

void ConditionLibrary::reseedScoreBand(int idx, float score) {
  // Sustained-floor re-anchor (2026-11-22 real-data tour): unlike the
  // regime-entry recalibration (the net re-converges, a real transient -
  // grace covers it), a machine floor drift inside one visit needs NO
  // grace: the net is already at the new floor, the score IS it. Re-anchor
  // the band MATURE at the current level so the gate tracks the drift
  // without opening a coverage gap.
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  c.s_n = cfg::PERCOND_MIN_N;
  c.s_mu = score;
  c.s_m2 = (0.10f * score) * (0.10f * score) * (float)c.s_n;
}

float ConditionLibrary::scoreTheta(int idx, float fallback) const {
  if (idx < 0 || idx >= n_) return fallback;
  const Condition& c = lib_[idx];
  if (c.s_n < cfg::PERCOND_MIN_N) return fallback;
  return c.s_mu + cfg::PERCOND_KSIG * sqrtf(c.s_m2 / (float)c.s_n);
}

void ConditionLibrary::observePos(int idx, float r_agg) {
  if (idx < 0 || idx >= n_) return;
  Condition& c = lib_[idx];
  if (c.r_n >= cfg::PERCOND_MIN_N) {
    float sd = sqrtf(c.r_m2 / (float)c.r_n);
    // outlier gate = the EFFECTIVE gate (Welford stats, floored at the
    // re-anchored wander floor): a draw past it never widens its own band.
    // The 2026-11-22 grow-only variant was tried and REVERTED: it absorbed
    // the suite's in-radius fault (the band widened toward the fault's
    // r_agg before the accumulator fired). The wander side it starved is
    // handled by the unit floor + the yardstick + the score-band re-anchor
    // + the subside-proven positional re-anchor (reseedPosBand, 2026-11-22b
    // stage-8: the block's internal warm-up re-alarmed its own rows in a
    // limit cycle under the frozen gate).
    if (r_agg > fmaxf(c.r_mu + cfg::PERCOND_KSIG * sd, c.r_floor))
      return;                       // anomaly: never widens its own band.
  }
  c.r_n++;
  float d = r_agg - c.r_mu;
  c.r_mu += d / (float)c.r_n;
  c.r_m2 += d * (r_agg - c.r_mu);
}

float ConditionLibrary::posTheta(int idx) const {
  if (idx < 0 || idx >= n_) return -1.0f;
  const Condition& c = lib_[idx];
  if (c.r_n < cfg::PERCOND_MIN_N) return -1.0f;   // gate inactive (grace)
  float gate = c.r_mu + cfg::PERCOND_KSIG * sqrtf(c.r_m2 / (float)c.r_n);
  return gate > c.r_floor ? gate : c.r_floor;     // the wander floor holds
}

void ConditionLibrary::reseedPosBand(int idx, float rag_max) {
  // Subside-proven positional re-anchor (node_config.h POS_EX_*): the
  // excursion was judged the state's own wander, so the gate must cover
  // it from this step on - MATURE, no grace, no coverage gap (a grace
  // window here would leave the gate inactive while the wander it covers
  // is still streaming). The Welford seeds at the peak; r_floor pins the
  // gate at peak x (1 + k x sd-frac) against the cool-row decay.
  if (idx < 0 || idx >= n_ || rag_max <= 0.0f) return;
  Condition& c = lib_[idx];
  c.r_n  = cfg::PERCOND_MIN_N;
  c.r_mu = rag_max;
  c.r_m2 = (cfg::POS_RESEED_SD_FRAC * rag_max) *
           (cfg::POS_RESEED_SD_FRAC * rag_max) * (float)c.r_n;
  c.r_floor = rag_max *
              (1.0f + cfg::PERCOND_KSIG * cfg::POS_RESEED_SD_FRAC);
}

bool ConditionLibrary::converged(int idx, float score) const {
  if (idx < 0 || idx >= n_) return false;
  const Condition& c = lib_[idx];
  if (c.s_n < cfg::PERCOND_MIN_N) return false;
  return score <= c.s_mu + 2.0f * sqrtf(c.s_m2 / (float)c.s_n);
}

float ConditionLibrary::matchDist(const float* raw, const FeatureSpace& fs,
                                  float q, int* idx) const {
  int   best = -1;
  float best_d = 1e9f;
  for (int i = 0; i < n_; ++i) {
    float worst = 0;
    bool  ok = true;
    for (int f = 0; f < nf_; ++f) {
      float unit = lib_[i].u[f] > 0 ? lib_[i].u[f] : fs.unit(f);
      float d = fabsf(fs.rel(f, raw[f]) - lib_[i].c[f]) / unit;
      if (d > q) { ok = false; break; }
      if (d > worst) worst = d;
    }
    if (ok && worst < best_d) { best = i; best_d = worst; }
  }
  if (idx) *idx = best;
  return best >= 0 ? best_d : -1.0f;
}

float ConditionLibrary::distTo(int idx, const float* raw,
                               const FeatureSpace& fs) const {
  if (idx < 0 || idx >= n_) return -1.0f;
  float worst = 0;
  for (int f = 0; f < nf_; ++f) {
    float unit = lib_[idx].u[f] > 0 ? lib_[idx].u[f] : fs.unit(f);
    float d = fabsf(fs.rel(f, raw[f]) - lib_[idx].c[f]) / unit;
    if (d > worst) worst = d;
  }
  return worst;
}

int ConditionLibrary::worstFeature(int idx, const float* raw,
                                   const FeatureSpace& fs) const {
  if (idx < 0 || idx >= n_) return -1;
  int worst_f = -1;
  float worst = -1.0f;
  for (int f = 0; f < nf_; ++f) {
    float unit = lib_[idx].u[f] > 0 ? lib_[idx].u[f] : fs.unit(f);
    float d = fabsf(fs.rel(f, raw[f]) - lib_[idx].c[f]) / unit;
    if (d > worst) { worst = d; worst_f = f; }
  }
  return worst_f;
}

int ConditionLibrary::nearest(const float* raw, const FeatureSpace& fs,
                              float* dist) const {
  // matchDist's loop without the radius cutoff: every condition qualifies,
  // the smallest worst-feature normalized distance wins.
  int   best = -1;
  float best_d = 1e9f;
  for (int i = 0; i < n_; ++i) {
    float worst = 0;
    for (int f = 0; f < nf_; ++f) {
      float unit = lib_[i].u[f] > 0 ? lib_[i].u[f] : fs.unit(f);
      float d = fabsf(fs.rel(f, raw[f]) - lib_[i].c[f]) / unit;
      if (d > worst) worst = d;
    }
    if (worst < best_d) { best = i; best_d = worst; }
  }
  if (dist) *dist = best >= 0 ? best_d : -1.0f;
  return best;
}

void ConditionLibrary::list() const {
  Serial.printf("[LIB] %d known condition(s):\n", n_);
  for (int i = 0; i < n_; ++i) {
    const Condition& c = lib_[i];
    Serial.printf("  %-16s n=%-4lu src=%-8s kind=%-6s theta=%.2f s_n=%lu"
                  " centroid=[", c.id,
                  (unsigned long)c.count, c.source,
                  c.kind == COND_NORMAL ? "normal" : "fault",
                  scoreTheta(i, 0.0f), (unsigned long)c.s_n);
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.4f", f ? " " : "", c.c[f]);
    Serial.printf("] units=[");
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.4f", f ? " " : "", c.u[f]);
    Serial.println("]");
  }
}

bool ConditionLibrary::save(const char* path) const {
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.println("[FS] conditions write FAILED"); return false; }
  for (int i = 0; i < n_; ++i) {
    const Condition& c = lib_[i];
    f.printf("{\"id\":\"%s\",\"c\":[", c.id);
    for (int k = 0; k < nf_; ++k)
      f.printf("%s%.4f", k ? "," : "", c.c[k]);
    f.printf("],\"u\":[");
    for (int k = 0; k < nf_; ++k)
      f.printf("%s%.4f", k ? "," : "", c.u[k]);
    f.printf("],\"count\":%lu,\"first\":%lu,\"last\":%lu,\"src\":\"%s\","
             "\"kind\":\"%s\",\"smu\":%.4f,\"sm2\":%.4f,\"sn\":%lu,"
             "\"rmu\":%.4f,\"rm2\":%.4f,\"rn\":%lu,\"rfloor\":%.4f}\n",
             (unsigned long)c.count, (unsigned long)c.first_seen,
             (unsigned long)c.last_seen, c.source,
             c.kind == COND_NORMAL ? "normal" : "fault",
             c.s_mu, c.s_m2, (unsigned long)c.s_n,
             c.r_mu, c.r_m2, (unsigned long)c.r_n);
  }
  f.close();
  return true;
}

bool ConditionLibrary::load(const char* path) {
  n_ = 0;
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  while (f.available() && n_ < cfg::LIB_MAX) {
    String line = f.readStringUntil('\n');
    if (line.length() < 10) continue;
    Condition& c = lib_[n_];
    memset(&c, 0, sizeof(c));
    jsonStr(line.c_str(), "id", c.id, sizeof(c.id));
    jsonStr(line.c_str(), "src", c.source, sizeof(c.source));
    char kindstr[8] = "";
    jsonStr(line.c_str(), "kind", kindstr, sizeof(kindstr));
    c.kind = (strcmp(kindstr, "normal") == 0) ? COND_NORMAL : COND_FAULT;
    const char* p = strstr(line.c_str(), "\"c\":[");
    if (p) {
      p += 5;
      // NOTE: strtof does NOT skip separators: without this guard the
      // 2nd..Nth centroid values silently load as 0 after a reboot
      // (a latent bug inherited from the vfd_streamq MVP libLoad).
      for (int k = 0; k < nf_; ++k) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p || *p == ']') break;
        c.c[k] = strtof(p, (char**)&p);
      }
    }
    p = strstr(line.c_str(), "\"u\":[");
    if (p) {                                  // old files: u stays 0
      p += 5;                                 // -> CAL unit fallback
      for (int k = 0; k < nf_; ++k) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p || *p == ']') break;
        c.u[k] = strtof(p, (char**)&p);
      }
    }
    c.count      = (uint32_t)jsonNum(line.c_str(), "count", 0);
    c.first_seen = (uint32_t)jsonNum(line.c_str(), "first", 0);
    c.last_seen  = (uint32_t)jsonNum(line.c_str(), "last", 0);
    // per-condition score band (2026-11-19): optional keys - old files
    // load with s_n = 0 and fall back to the global theta until re-learned
    c.s_mu       = jsonNum(line.c_str(), "smu", 0);
    c.s_m2       = jsonNum(line.c_str(), "sm2", 0);
    c.s_n        = (uint32_t)jsonNum(line.c_str(), "sn", 0);
    // per-condition positional band (2026-11-21, cond-obs era): optional
    // keys - old files load with r_n = 0 (gate inactive until re-learned)
    c.r_mu       = jsonNum(line.c_str(), "rmu", 0);
    c.r_m2       = jsonNum(line.c_str(), "rm2", 0);
    c.r_n        = (uint32_t)jsonNum(line.c_str(), "rn", 0);
    // positional wander floor (2026-11-22b): optional - old files load 0
    c.r_floor    = jsonNum(line.c_str(), "rfloor", 0);
    if (c.id[0]) n_++;
  }
  f.close();
  return true;
}
