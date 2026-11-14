// =====================================================================
// interactive_host.cpp -- multi-node host run for the web bench.
// N REAL SensorNodeApps (Arduino stubs) watching ONE simulated motor:
// the user drives the machine state from the web panel, and each node
// renders the CURRENT state through its own modality:
//
//   node 0: vfd_g100  (drive side: current, slip, DC bus, torque)
//   node 1: vib_adxl  (vibration: RMS, FFT band powers, dominant f)
//   further nodes alternate the two profiles.
//
// The user's loop: inject a fault -> the nodes catch it -> label the ask
// -> inject again -> recognised locally, no ask.
//
// Collective layer ("the nodes work together"):
//   - when a node opens an UNKNOWN condition, the system consults the
//     siblings and emits a verdict line:
//       known        a same-profile sibling's library matches the means
//       corroborated a sibling is ALARMING at the same time (real fault)
//       unconfirmed  siblings are alive and all report normal - their
//                    "normal" may be blindness to this modality, so this
//                    is evidence, NOT a conclusion; the user decides
//       unconsulted  no siblings (single node) or none ready
//   - a user label is applied to the asking node and SHARED: siblings
//     with an open unknown ask get the same slug (each builds its own
//     centroid from its own episode means); same-profile siblings
//     additionally get the centroid means mapped through their own space.
//
// Stdin protocol (the web server is the pacing brain):
//   s                       advance every node by ONE monitor step
//   F <STATE>               set the machine state:
//                           NONE|SPEED|IMBALANCE|BEARING|HEAVY
//   L <idx> <slug> <json>   applyFeedback(json) on node idx + sharing
//   q                       quit
// Per monitor step: one state line PER node:
//   S <node> <step> <phase> <score> <theta> <ep_open> <cond> <lib> f0 ...
// plus M <state> whenever the machine state changes. Event lines:
// [RL]/[EP]/[FB]/[LIB]/[SHARE]/[AUTO]/VERDICT/[MACHINE].
// META + READY open the stream; DONE closes it.
//
// Env:
//   TINYRL_STEPS=<n>     monitor steps (default 900)
//   TINYRL_NODES=<n>     nodes (default 2; 1 = single-node mode)
//   TINYRL_AUTOAGENT=1   scripted agent answers asks; the machine state
//                        self-injects on the CI timetable (200 speed,
//                        380 imbalance, 560 bearing, 740 heavy)
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sensor_interface.h"
#include "node_runtime.h"

// ---- deterministic noise (per-node LCG state) -------------------------
static float noise(unsigned long& lcg, float scale) {
  lcg = lcg * 1664525u + 1013904223u;
  float u = (float)(lcg % 10000) / 10000.0f - 0.5f;
  return 2.0f * scale * u;
}

// ---- the two real node profiles ---------------------------------------
static constexpr FeatureInfo FEATS_G100[4] = {
  {"i", "A"}, {"slip", "Hz"}, {"vdc", "V"}, {"tq", "%"},
};
static constexpr FeatureProfile PROF_G100 = {"vfd_g100", 4, FEATS_G100};
static constexpr FeatureInfo FEATS_VIB[6] = {
  {"rms", "g"}, {"b1", "g2"}, {"b2", "g2"}, {"b3", "g2"}, {"b4", "g2"},
  {"fmax", "Hz"},
};
static constexpr FeatureProfile PROF_VIB = {"vib_adxl", 6, FEATS_VIB};

// ---- machine truth: ONE motor, the USER injects its state ------------
// The web UI drives it (F commands); CI self-injects on the timetable.
// Each node renders the CURRENT machine state through its own modality.
enum Ev { NONE, SPEED, IMBALANCE, BEARING, HEAVY };
static Ev machine_state = NONE;
static const char* truth_name(Ev e) {
  switch (e) {
    case SPEED:     return "op_speed_2";
    case IMBALANCE: return "imbalance";
    case BEARING:   return "bearing";
    case HEAVY:     return "fault_heavy";
    default:        return "normal";
  }
}

class ScriptedSensor : public ISensorNode {
public:
  void setProfile(const FeatureProfile* p) { prof_ = p; }
  void setSeed(unsigned long s) { lcg_ = s; }

  bool begin() override { step_ = 0; return true; }
  void poll() override { step_++; scenario(step_); }
  uint32_t pollPeriodMs() const override { return 1000; }
  bool fresh() const override { return step_ > 0; }
  uint32_t lastOkMs() const override { return millis(); }
  uint32_t failCount() const override { return 0; }
  const FeatureProfile& profile() const override { return *prof_; }
  void features(float* out) const override {
    for (int i = 0; i < prof_->n_features; ++i) out[i] = f_[i];
  }
  size_t logLine(char* out, size_t cap) override {
    int o = snprintf(out, cap, "[SIM] t=%ld", step_);
    for (int i = 0; i < prof_->n_features && o < (int)cap; ++i)
      o += snprintf(out + o, cap - o, " %.4g", f_[i]);
    return o;
  }

  // scripted-truth hint for the CI agent = the CURRENT machine state
  const char* anomalyLabel() const { return truth_name(machine_state); }

private:
  void scenario(long t) {
    Ev e = machine_state;
    if (prof_ == &PROF_G100) {
      // drive side: thermal drift + noise, then the event's drive response
      float warm = 1.0f - expf(-t / 900.0f);
      f_[0] = 3.0f + 0.05f * warm + noise(lcg_, 0.05f);
      f_[1] = 0.25f + 0.008f * warm + noise(lcg_, 0.03f);
      f_[2] = 585.0f + noise(lcg_, 1.0f);
      f_[3] = 45.0f + 0.30f * warm + noise(lcg_, 1.5f);
      if (e == SPEED) {      // setpoint move: drive features shift and stay
        f_[0] += 0.10f; f_[1] += 0.06f; f_[3] += 2.0f; f_[2] -= 1.0f;
      }
      if (e == IMBALANCE) {  // mechanical: the drive barely notices
        f_[0] += 0.05f; f_[3] += 1.2f;
      }
      if (e == HEAVY) { f_[0] += 0.20f; f_[3] += 4.0f; f_[2] -= 4.0f; }
      // BEARING: drive-side blind by construction
    } else {
      // vibration side: rms ~ 0.035 g, band powers scale with rms^2,
      // fmax = 1x running speed (~24 Hz)
      float rms = 0.035f * (1.0f + noise(lcg_, 0.2f));
      float b[4], c[4] = {0.0025f, 0.0010f, 0.0004f, 0.00015f};
      for (int k = 0; k < 4; k++)
        b[k] = c[k] * (rms / 0.035f) * (rms / 0.035f) * (1.0f + noise(lcg_, 0.4f));
      float fmax = 24.2f + noise(lcg_, 1.5f);
      if (e == IMBALANCE) {  // 1x-speed band dominates
        rms *= 1.5f; b[0] *= 2.5f;
      }
      if (e == BEARING) {    // high bands + defect tone
        rms *= 1.15f; b[2] *= 3.0f; b[3] *= 4.0f; fmax = 28.0f + noise(lcg_, 2.0f);
      }
      if (e == HEAVY) {      // everything lights up
        rms *= 3.0f; b[0] *= 3.0f; b[1] *= 2.5f; b[2] *= 2.5f; b[3] *= 3.0f;
      }
      f_[0] = rms;
      f_[1] = b[0]; f_[2] = b[1]; f_[3] = b[2]; f_[4] = b[3];
      f_[5] = fmax;
      // SPEED: the vibration signature is unchanged (setpoint trim, same
      // load) - the modality split the collective layer exists to expose
    }
  }

  const FeatureProfile* prof_ = &PROF_G100;
  unsigned long lcg_ = 12345;
  float f_[8] = {0};
  long step_ = 0;
};

// ---- one node rig ------------------------------------------------------
struct Rig {
  ScriptedSensor sensor;
  SensorNodeApp  app;
  long last_step = 0;
  bool consulted = false;      // verdict emitted for the current ask
  // scripted-agent state (CI mode only)
  int  ask_age = 0;
  bool asked = false;
};

static Rig rigs[8];
static int n_nodes = 2;

static void print_state();

// scripted agent (CI mode): answer an ask 3 steps after it opens, using
// the machine truth as the label hint.
static void autoagent(Rig& r, int idx) {
  if (!r.app.episodeOpen()) {
    r.ask_age = 0; r.asked = false;
    return;
  }
  if (strcmp(r.app.conditionId(), "unknown") == 0) {
    if (r.ask_age >= 3 && !r.asked) {
      const char* lb = r.sensor.anomalyLabel();
      char j[96];
      if (lb) {
        snprintf(j, sizeof(j),
                 "{\"action\":\"label\",\"label\":\"%s\",\"by\":\"agent\"}", lb);
      } else {
        snprintf(j, sizeof(j), "{\"action\":\"normal\",\"by\":\"agent\"}");
      }
      r.app.applyFeedback(j);
      r.asked = true;
      // CI sharing: sibling with an open unknown ask gets the same slug
      for (int k = 0; k < n_nodes; ++k) {
        if (k == idx) continue;
        if (rigs[k].app.episodeOpen()
            && strcmp(rigs[k].app.conditionId(), "unknown") == 0) {
          rigs[k].app.applyFeedback(j);
          printf("[SHARE] '%s' -> node %d (corroborated ask)\n", lb, k);
        }
      }
    } else {
      r.ask_age++;
    }
  } else {
    r.ask_age = 0;
  }
}

// ---- collective layer --------------------------------------------------
// Consult the siblings for the asking node's open unknown episode and
// emit ONE verdict line per ask. The verdict arms the UI; it never
// auto-resolves row 3 (siblings-normal is evidence, not a conclusion).
static void consult(int idx) {
  Rig& r = rigs[idx];
  float means[8];
  r.app.episodeMeans(means);
  const FeatureProfile& myprof = r.sensor.profile();

  // 1) same-profile sibling library match -> the collective knows it
  for (int j = 0; j < n_nodes; ++j) {
    if (j == idx) continue;
    if (&rigs[j].sensor.profile() != &myprof) continue;
    int m = rigs[j].app.libMatch(means);
    if (m >= 0) {
      const char* slug = rigs[j].app.libName(m);
      char j_[96];
      snprintf(j_, sizeof(j_),
               "{\"action\":\"label\",\"label\":\"%s\",\"by\":\"agent-net\"}",
               slug);
      printf("[AUTO] node %d informed by node %d's library ('%s')\n",
             idx, j, slug);
      r.app.applyFeedback(j_);
      print_state();
      return;
    }
  }

  // 2) corroboration: is any sibling ALARMING right now?
  int alarmers = 0;
  char states[128] = "";
  size_t o = 0;
  bool any_ready = false;
  for (int j = 0; j < n_nodes; ++j) {
    if (j == idx) continue;
    bool ready = strcmp(rigs[j].app.phase(), "cal") != 0
              && strcmp(rigs[j].app.phase(), "fit") != 0
              && strcmp(rigs[j].app.phase(), "idle") != 0;
    if (!ready) continue;
    any_ready = true;
    bool al = strcmp(rigs[j].app.phase(), "alarm") == 0;
    if (al) alarmers++;
    o += snprintf(states + o, sizeof(states) - o, "node%d=%s ", j,
                  al ? "ALARMING" : "normal");
  }
  if (!any_ready) {
    printf("VERDICT %d unconsulted no-ready-siblings\n", idx);
  } else if (alarmers > 0) {
    printf("VERDICT %d corroborated %s\n", idx, states);
  } else {
    printf("VERDICT %d unconfirmed %s\n", idx, states);
  }
  r.consulted = true;
}

static void print_state() {
  for (int i = 0; i < n_nodes; ++i) {
    float f[8];
    rigs[i].sensor.features(f);
    printf("S %d %ld %s %.4f %.4f %d %s %u",
           i, rigs[i].last_step, rigs[i].app.phase(), rigs[i].app.score(),
           rigs[i].app.theta(), rigs[i].app.episodeOpen() ? 1 : 0,
           rigs[i].app.conditionId(), rigs[i].app.libSize());
    for (int k = 0; k < rigs[i].sensor.profile().n_features; ++k)
      printf(" %.5g", f[k]);
    printf("\n");
  }
}

static void step_all(bool autoagent_on) {
  // CI self-injection: the timetable stands in for a human at the panel
  // (inject 20 steps, revert, let the episode clear, next fault)
  if (autoagent_on) {
    static const struct { long at; Ev ev; } inj[] = {
      {200, SPEED}, {220, NONE}, {380, IMBALANCE}, {400, NONE},
      {560, BEARING}, {580, NONE}, {740, HEAVY}, {760, NONE},
    };
    static int inj_idx = 0;
    if (inj_idx < 8 && rigs[0].last_step >= inj[inj_idx].at) {
      machine_state = inj[inj_idx].ev;
      printf("M %s\n", truth_name(machine_state));
      printf("[MACHINE] %s\n", truth_name(machine_state));
      inj_idx++;
    }
  }
  for (int i = 0; i < n_nodes; ++i) {
    long before = rigs[i].app.stepCount();
    while (rigs[i].app.stepCount() == before) rigs[i].app.tick();
    rigs[i].last_step = (long)rigs[i].app.stepCount();
    if (autoagent_on) autoagent(rigs[i], i);
  }
  print_state();
  for (int i = 0; i < n_nodes; ++i) {
    if (rigs[i].app.episodeOpen()
        && strcmp(rigs[i].app.conditionId(), "unknown") == 0) {
      if (!rigs[i].consulted) consult(i);
    } else {
      rigs[i].consulted = false;   // new ask -> consult again
    }
  }
}

// share a user label: asking node first, then the collective rules
static void set_machine(const char* name) {
  Ev e = NONE;
  if (strcmp(name, "SPEED") == 0) e = SPEED;
  else if (strcmp(name, "IMBALANCE") == 0) e = IMBALANCE;
  else if (strcmp(name, "BEARING") == 0) e = BEARING;
  else if (strcmp(name, "HEAVY") == 0) e = HEAVY;
  else if (strcmp(name, "NONE") != 0 && strcmp(name, "NORMAL") != 0) return;
  machine_state = e;
  printf("M %s\n", truth_name(machine_state));
  printf("[MACHINE] set to %s\n", truth_name(machine_state));
}

static void do_label(int idx, const char* slug, const char* json) {
  bool open = rigs[idx].app.episodeOpen();
  rigs[idx].app.applyFeedback(json);
  if (!open) { print_state(); return; }
  for (int j = 0; j < n_nodes; ++j) {
    if (j == idx) continue;
    // a sibling stuck in the SAME unknown ask (corroborated) gets the
    // slug applied to its own episode: one label, each node learns from
    // its own means
    if (rigs[j].app.episodeOpen()
        && strcmp(rigs[j].app.conditionId(), "unknown") == 0) {
      rigs[j].app.applyFeedback(json);
      printf("[SHARE] '%s' -> node %d (corroborated ask)\n", slug, j);
      continue;
    }
    // same-profile sibling: share the asking node's episode means as a
    // centroid mapped through the sibling's own frozen space
    if (&rigs[j].sensor.profile() == &rigs[idx].sensor.profile()
        && slug[0]) {
      float means[8];
      rigs[idx].app.episodeMeans(means);
      int s = rigs[j].app.shareLabel(slug, means);
      printf("[SHARE] centroid '%s' -> node %d (lib idx %d)\n", slug, j, s);
    }
    // mixed-profile siblings: slug only (vocabulary); centroids are
    // meaningless across feature spaces
  }
  print_state();   // the ask may be resolved now - un-hold the server
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);

  long total = 900;
  if (const char* e = getenv("TINYRL_STEPS")) {
    total = atol(e);
    if (total < 100) total = 100;
  }
  n_nodes = 2;
  if (const char* e = getenv("TINYRL_NODES")) {
    n_nodes = atoi(e);
    if (n_nodes < 1) n_nodes = 1;
    if (n_nodes > 8) n_nodes = 8;
  }
  bool autoagent_on = getenv("TINYRL_AUTOAGENT") != nullptr;
  const char* p = getenv("TINYRL_PROFILE");   // single-node profile override

  for (int i = 0; i < n_nodes; ++i) {
    const FeatureProfile* prof =
        (n_nodes == 1 && p && strcmp(p, "vib") == 0) ? &PROF_VIB
        : (n_nodes == 1 && p && strcmp(p, "g100") == 0) ? &PROF_G100
        : (i % 2 == 0 ? &PROF_G100 : &PROF_VIB);
    rigs[i].sensor.setProfile(prof);
    rigs[i].sensor.setSeed(12345u + 7919u * (unsigned)i);
    NodeIdentity id{};
    char dev[32], base[64];
    snprintf(dev, sizeof(dev), "%s-%d",
             prof == &PROF_VIB ? "host-vib" : "host-g100", i);
    snprintf(base, sizeof(base), "v1/host/bench-%d", i);
    id.dev_id    = dev;
    id.uns_base  = base;
    id.wifi_ssid = "bench";
    id.wifi_pass = "bench";
    id.mqtt_host = "127.0.0.1";
    id.mqtt_port = 1883;
    rigs[i].app.begin(rigs[i].sensor, id);
    rigs[i].app.startLearning();
  }

  printf("META nodes=%d steps=%ld", n_nodes, total);
  for (int i = 0; i < n_nodes; ++i)
    printf(" node%d=%s nf=%d", i,
           rigs[i].sensor.profile().node_type,
           rigs[i].sensor.profile().n_features);
  printf("\nREADY\n");

  char line[256];
  bool run = true;
  while (run && std::fgets(line, sizeof(line), stdin)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n') line[n - 1] = 0;
    if (line[0] == 's') {
      step_all(autoagent_on);
    } else if (line[0] == 'F') {
      set_machine(line + 2);
    } else if (line[0] == 'L') {
      int idx = atoi(line + 2);
      if (idx < 0 || idx >= n_nodes) continue;
      char* sp = strchr(line + 2, ' ');        // after idx
      if (!sp) continue;
      char* slug = sp + 1;
      char* sp2 = strchr(slug, ' ');           // after slug
      if (!sp2) continue;
      *sp2 = 0;
      do_label(idx, slug, sp2 + 1);
    } else if (line[0] == 'q') {
      run = false;
    }
  }

  for (int i = 0; i < n_nodes; ++i)
    printf("DONE node=%d steps=%ld lib=%u\n", i, rigs[i].last_step,
           rigs[i].app.libSize());
  return 0;
}
