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
  for (int i = 0; i < n_; ++i) {
    bool ok = true;
    for (int f = 0; f < nf_; ++f) {
      float unit = lib_[i].u[f] > 0 ? lib_[i].u[f] : fs.unit(f);
      float d = fabsf(fs.rel(f, raw[f]) - lib_[i].c[f]);
      if (d > q * unit) { ok = false; break; }
    }
    if (ok) return i;   // first condition fully inside the radius
  }
  return -1;
}

int ConditionLibrary::label(const char* slug, const float* mean_rel,
                            const float* sd_rel, const char* by,
                            CondKind kind) {
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

void ConditionLibrary::list() const {
  Serial.printf("[LIB] %d known condition(s):\n", n_);
  for (int i = 0; i < n_; ++i) {
    const Condition& c = lib_[i];
    Serial.printf("  %-16s n=%-4lu src=%-8s centroid=[", c.id,
                  (unsigned long)c.count, c.source);
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.2f", f ? " " : "", c.c[f]);
    Serial.printf("] units=[");
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.2f", f ? " " : "", c.u[f]);
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
             "\"kind\":\"%s\"}\n",
             (unsigned long)c.count, (unsigned long)c.first_seen,
             (unsigned long)c.last_seen, c.source,
             c.kind == COND_NORMAL ? "normal" : "fault");
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
    if (c.id[0]) n_++;
  }
  f.close();
  return true;
}
