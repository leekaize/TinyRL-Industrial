// =====================================================================
// replay.cpp -- run a captured serial session through the REAL core.
//
//   bash tests/replay.sh data/serial/vib-01_<stamp>.log
//
// Input: the raw capture written by tools/seriallog.py (the .log file).
//   [HDR] line   -> feature identity (names/units/order) for the fake sensor
//   [RAW] lines  -> one sensor sample each, 2 s apart (the capture cadence)
//   command lines-> the same commands at the same point in the stream:
//     "[FB] action='start'"                            -> {"action":"start"}
//     "[FB] action='normal'" / "'fault'"               -> same action
//     "[FB] batch P00xx labeled 'slug' (kind)"         -> batch label cmd
//     "[FB] action='label' label='s' kind='k' by='b'"  -> live label cmd,
//        only when the session never batch-labeled that slug
//     "[FS] back to IDLE"                              -> factory reset
//
// Fresh-commissioning semantics: replay ALWAYS starts from an empty
// model and re-runs the session (learn at the captured start moment).
// If the capture never shows a start, learning starts at the first
// sample - so a capture from a node that booted with a restored model
// replays as a full commissioning instead.
//
// Output: the node's own serial log, plus one [REPLAY] line per sample
// where the decision changed or diverges from the capture, and a match
// summary. Workflow: change a cfg:: knob, rerun, watch the match rate.
// =====================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <Arduino.h>
#include <string>
#include <vector>
#include <set>
#include "node_runtime.h"

namespace {

struct Cap {
  uint32_t ts_ms = 0;
  float f[cfg::MAX_FEATURES] = {0};
  char state[16] = "", cond[32] = "";      // captured ground truth
};

char g_dev[32] = "node-01";
char g_type[24] = "unknown";
std::vector<std::string> g_feat_names, g_feat_units;

bool extract(const char* line, const char* key, char* out, size_t cap) {
  char pat[32];
  snprintf(pat, sizeof(pat), "%s='", key);
  const char* p = strstr(line, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while (*p && *p != '\'' && o < cap - 1) out[o++] = *p++;
  out[o] = 0;
  return true;
}

// sensor that serves captured samples as its perception
class ReplaySensor : public ISensorNode {
public:
  bool begin() override { return true; }
  void poll() override {}
  uint32_t pollPeriodMs() const override { return 1000; }
  bool fresh() const override { return have_; }
  uint32_t lastOkMs() const override { return millis(); }
  uint32_t failCount() const override { return 0; }
  const FeatureProfile& profile() const override { return prof_; }
  void features(float* out) const override {
    for (int i = 0; i < prof_.n_features; ++i) out[i] = cur_[i];
  }
  size_t logLine(char* out, size_t) override { out[0] = 0; return 0; }  // tick prints [RAW]

  void setIdentity() {
    static char names[cfg::MAX_FEATURES][12], units[cfg::MAX_FEATURES][8];
    for (size_t i = 0; i < g_feat_names.size() && i < cfg::MAX_FEATURES; ++i) {
      snprintf(names[i], sizeof(names[i]), "%s", g_feat_names[i].c_str());
      snprintf(units[i], sizeof(units[i]), "%s", g_feat_units[i].c_str());
    }
    for (int i = 0; i < cfg::MAX_FEATURES; ++i) infos_[i] = {names[i], units[i]};
    prof_ = {g_type, (int)g_feat_names.size(), infos_};
  }
  void setSample(const Cap& s) {
    for (int i = 0; i < prof_.n_features; ++i) cur_[i] = s.f[i];
    have_ = true;
  }

private:
  FeatureProfile prof_{};
  FeatureInfo infos_[cfg::MAX_FEATURES] = {};
  float cur_[cfg::MAX_FEATURES] = {0};
  bool have_ = false;
};

bool extract_kv(const char* line, const char* key, char* out, size_t cap) {
  char pat[40];
  snprintf(pat, sizeof(pat), " %s=", key);
  const char* p = strstr(line, pat);
  if (!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while (*p && *p != ' ' && *p != '\n' && o < cap - 1) out[o++] = *p++;
  out[o] = 0;
  return true;
}

bool parse_hdr(const char* line) {
  char feats[192] = "";
  char dev[32] = "", type[24] = "";
  extract_kv(line, "dev", dev, sizeof(dev));
  extract_kv(line, "type", type, sizeof(type));
  if (!extract_kv(line, "features", feats, sizeof(feats))) return false;
  if (dev[0]) snprintf(g_dev, sizeof(g_dev), "%s", dev);
  if (type[0]) snprintf(g_type, sizeof(g_type), "%s", type);
  g_feat_names.clear();
  g_feat_units.clear();
  char* tok = strtok(feats, ",");
  while (tok) {
    char* colon = strchr(tok, ':');
    if (colon) { *colon = 0; g_feat_units.push_back(colon + 1); }
    else g_feat_units.push_back("");
    g_feat_names.push_back(tok);
    tok = strtok(nullptr, ",");
  }
  return !g_feat_names.empty();
}

bool parse_raw(const char* line, Cap& s) {
  const char* p = strstr(line, "ts_ms=");
  if (!p) return false;
  s.ts_ms = (uint32_t)strtoul(p + 6, nullptr, 10);
  char name[16], val[32];
  for (size_t i = 0; i < g_feat_names.size() && i < cfg::MAX_FEATURES; ++i) {
    snprintf(name, sizeof(name), " %s=", g_feat_names[i].c_str());
    const char* q = strstr(line, name);
    if (!q) return false;
    q += strlen(name);
    size_t o = 0;
    while (*q && *q != ' ' && o < sizeof(val) - 1) val[o++] = *q++;
    val[o] = 0;
    s.f[i] = strtof(val, nullptr);
  }
  extract_kv(line, "state", s.state, sizeof(s.state));
  extract_kv(line, "cond", s.cond, sizeof(s.cond));
  return true;
}

bool pass1(FILE* fp, bool* has_start, std::vector<std::string>* slugs,
           std::vector<std::string>* kinds) {
  char line[512];
  while (fgets(line, sizeof(line), fp)) {
    if (line[0] != '[') continue;
    if (strncmp(line, "[HDR]", 5) == 0) parse_hdr(line);
    else if (strncmp(line, "[FB] action='start'", 19) == 0) *has_start = true;
    else if (strncmp(line, "[FB] batch", 10) == 0) {
      char slug[32] = "", kind[8] = "";
      const char* p = strstr(line, "labeled '");
      if (p) sscanf(p + 9, "%31[^']", slug);
      const char* k = strchr(line, '(');
      if (k) sscanf(k + 1, "%7[^)]", kind);
      if (slug[0]) { slugs->push_back(slug); kinds->push_back(kind); }
    }
  }
  return !slugs->empty();
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: replay <capture.log>\n"
                    "  capture made with: python3 tools/seriallog.py\n");
    return 1;
  }
  FILE* fp = fopen(argv[1], "r");
  if (!fp) { perror(argv[1]); return 1; }

  bool has_start = false;
  std::vector<std::string> lab_slugs, lab_kinds;
  pass1(fp, &has_start, &lab_slugs, &lab_kinds);
  if (g_feat_names.empty()) {
    fprintf(stderr, "[REPLAY] no [HDR] line in capture - recorded with pre-RAW firmware?\n");
    fclose(fp);
    return 1;
  }

  ReplaySensor sen;
  sen.setIdentity();
  NodeIdentity id{};
  id.dev_id = g_dev;
  id.uns_base = "v1/Replay/Only";
  id.wifi_ssid = "x"; id.wifi_pass = "x";
  id.mqtt_host = "127.0.0.1"; id.mqtt_port = 1883;
  SensorNodeApp app;
  app.begin(sen, id);                        // fresh stub FS -> IDLE
  if (!has_start) {
    printf("[REPLAY] no start in capture - learning starts at the first sample\n");
    app.applyFeedback("{\"action\":\"start\",\"by\":\"replay\"}");
  }

  int n = 0, match = 0, match_post = 0, n_post = 0, guard_max = 0;
  char prev_key[64] = "";
  // captured batch labels are remapped IN ORDER onto the replay's own
  // stored batches: a fresh trajectory matures its asks a few samples
  // off the captured timing, so the literal captured batch id may not
  // exist yet (or already be gone). Order is the meaningful part.
  size_t lab_idx = 0;
  uint32_t last_pend = 0;
  char line[512];
  Cap s;
  rewind(fp);
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "[RAW]", 5) == 0) {
      if (!parse_raw(line, s)) continue;
      sen.setSample(s);
      // drive exactly one capture cadence (2 s = 2 monitor steps)
      unsigned long t0 = millis();
      int guard = 0;
      while (millis() - t0 < 2000 && guard++ < 4000) app.tick();
      if (guard > guard_max) guard_max = guard;

      if (app.pendingSize() > last_pend && lab_idx < lab_slugs.size()) {
        char bid[12] = "";
        app.pendingAt((int)app.pendingSize() - 1, bid, sizeof(bid),
                      nullptr, nullptr);
        char j[160];
        snprintf(j, sizeof(j),
                 "{\"action\":\"label\",\"label\":\"%s\",\"kind\":\"%s\","
                 "\"batch\":\"%s\",\"by\":\"replay\"}",
                 lab_slugs[lab_idx].c_str(), lab_kinds[lab_idx].c_str(), bid);
        printf("[REPLAY] label '%s' (%s) -> replay batch %s\n",
               lab_slugs[lab_idx].c_str(), lab_kinds[lab_idx].c_str(), bid);
        app.applyFeedback(j);
        ++lab_idx;
      }
      last_pend = app.pendingSize();

      // captured state= is mon_.stateString() (private); map it from the
      // public phase(): idle->idle, cal/fit->learn, mon->ok, alarm->alarm
      const char* ph = app.phase();
      const char* st = strcmp(ph, "alarm") == 0 ? "alarm"
                     : strcmp(ph, "mon")   == 0 ? "ok"
                     : strcmp(ph, "idle")  == 0 ? "idle" : "learn";
      char key[64];
      snprintf(key, sizeof(key), "%s|%s", st, app.conditionId());
      char cap_key[64];
      snprintf(cap_key, sizeof(cap_key), "%s|%s", s.state, s.cond);
      bool same = strcmp(key, cap_key) == 0;
      ++n;
      if (same) ++match;
      const char* ph2 = app.phase();
      bool ready = strcmp(ph2, "mon") == 0 || strcmp(ph2, "alarm") == 0;
      if (ready) { ++n_post; if (same) ++match_post; }
      if (strcmp(key, prev_key) != 0 || !same) {
        printf("[REPLAY] t=%lus state=%s cond=%-14s | captured: %s%s\n",
               (unsigned long)(s.ts_ms / 1000), st, app.conditionId(),
               cap_key, same ? "" : "   <-- diverges");
      }
      snprintf(prev_key, sizeof(prev_key), "%s", key);
    } else if (strncmp(line, "[FB] action='start'", 19) == 0) {
      app.applyFeedback("{\"action\":\"start\",\"by\":\"replay\"}");
    } else if (strncmp(line, "[FB] action='normal'", 20) == 0) {
      app.applyFeedback("{\"action\":\"normal\",\"by\":\"replay\"}");
    } else if (strncmp(line, "[FB] action='fault'", 19) == 0) {
      app.applyFeedback("{\"action\":\"fault\",\"by\":\"replay\"}");
    } else if (strncmp(line, "[FB] batch", 10) == 0) {
      // applied via the ordered remap above, not from the stream
    } else if (strncmp(line, "[FB] action='label'", 19) == 0) {
      char label[32] = "", kind[8] = "", by[20] = "";
      extract(line, "label", label, sizeof(label));
      extract(line, "kind", kind, sizeof(kind));
      extract(line, "by", by, sizeof(by));
      bool covered_by_batch = false;
      for (const auto& s : lab_slugs)
        if (s == label) { covered_by_batch = true; break; }
      if (label[0] && !covered_by_batch) {                // live-only label
        char j[160];
        if (kind[0])
          snprintf(j, sizeof(j),
                   "{\"action\":\"label\",\"label\":\"%s\",\"kind\":\"%s\","
                   "\"by\":\"%s\"}", label, kind, by[0] ? by : "replay");
        else
          snprintf(j, sizeof(j),
                   "{\"action\":\"label\",\"label\":\"%s\",\"by\":\"%s\"}",
                   label, by[0] ? by : "replay");
        app.applyFeedback(j);
      }
    } else if (strncmp(line, "[FS] back to IDLE", 17) == 0) {
      app.factoryReset();
    }
  }
  fclose(fp);

  printf("\n[REPLAY] samples=%d (max drive-loop guard=%d)\n", n, guard_max);
  if (n) {
    printf("[REPLAY] state+cond match: %d/%d (%.1f%%)",
           match, n, 100.0 * match / n);
    if (n_post)
      printf(" | post-commissioning: %d/%d (%.1f%%)",
             match_post, n_post, 100.0 * match_post / n_post);
    printf("\n[REPLAY] diverging moments are flagged '<-- diverges' above; "
           "a knob change moves these.\n");
  }
  return 0;
}
