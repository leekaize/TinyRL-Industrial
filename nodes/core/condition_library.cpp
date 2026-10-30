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

int ConditionLibrary::match(const float* raw, const FeatureSpace& fs) const {
  int best = -1;
  float bestd = cfg::LIB_MATCH_D;               // mean-distance threshold
  for (int i = 0; i < n_; ++i) {
    float d2 = 0;
    for (int f = 0; f < nf_; ++f) {
      float d = fs.to01(f, raw[f]) - lib_[i].c[f];
      d2 += d * d;
    }
    float d = sqrtf(d2 / (float)nf_);
    if (d < bestd) { bestd = d; best = i; }
  }
  return best;
}

int ConditionLibrary::label(const char* slug, const float* mean_raw,
                            const FeatureSpace& fs, const char* by) {
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
    for (int f = 0; f < nf_; ++f) c.c[f] = fs.to01(f, mean_raw[f]);
    Serial.printf("[LIB] learned new condition '%s'\n", slug);
  } else if (found >= 0) {
    Condition& c = lib_[found];
    for (int f = 0; f < nf_; ++f)
      c.c[f] = 0.8f * c.c[f] + 0.2f * fs.to01(f, mean_raw[f]);
    c.count++;
    c.last_seen = millis();
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
    c.c[f] = 0.9f * c.c[f] + 0.1f * fs.to01(f, mean_raw[f]);
  c.count++;
  c.last_seen = millis();
}

void ConditionLibrary::list() const {
  Serial.printf("[LIB] %d known condition(s):\n", n_);
  for (int i = 0; i < n_; ++i) {
    Serial.printf("  %-16s n=%-4lu src=%-8s centroid=[",
                  lib_[i].id, (unsigned long)lib_[i].count, lib_[i].source);
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.2f", f ? " " : "", lib_[i].c[f]);
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
    f.printf("],\"count\":%lu,\"first\":%lu,\"last\":%lu,\"src\":\"%s\"}\n",
             (unsigned long)c.count, (unsigned long)c.first_seen,
             (unsigned long)c.last_seen, c.source);
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
    const char* p = strstr(line.c_str(), "\"c\":[");
    if (p) {
      p += 5;
      // NOTE: strtof does NOT skip separators: without this guard the
      // 2nd..Nth centroid values silently load as 0 after a reboot
      // (a latent bug inherited from the vfd_streamq MVP libLoad).
      for (int k = 0; k < nf_; ++k) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p) break;
        c.c[k] = strtof(p, (char**)&p);
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
