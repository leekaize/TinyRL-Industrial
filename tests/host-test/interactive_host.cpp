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
//   s                       advance every node by ONE monitor step (1 s)
//   F <CMD>                 drive the machine: START|STOP|SPEED1|SPEED2|
//                           IMBALANCE|LOOSE_MOUNT|PHASE_LOSS|NORMAL
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
//   TINYRL_AUTOAGENT=1   scripted agent answers asks; the machine self-
//                        injects on the CI timetable (40 start, 160 stop,
//                        200 start, faults at 300/480/660 + recoveries)
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

// ---- machine truth: ONE motor, the USER drives it from the panel -----
// Model: running on/off, a speed setpoint, an optional fault overlay.
// Three normal conditions only (user decision 2026-11-14):
//   op_stopped (idle), op_speed_1 (5 Hz), op_speed_2 (20 Hz)
//   faults:            imbalance, loose_mount, phase_loss (VFD -> motor)
// Fault asks are labelled WITH their operating point (compound slug,
// e.g. imbalance@op_speed_1): faults under different setpoints are
// different library conditions.
// Start/stop and setpoint moves ramp the ACTUAL speed over ~2 s (VFD
// accel/decel). The set-frequency register reports the COMMANDED setpoint
// directly (user fact: no motor feedback needed) - it steps instantly and
// reads 0 while stopped. Drive slip is gone from the feature set.
static constexpr float SP_HZ[2] = {5.0f, 20.0f};
enum Ev { STOPPED, SPEED1, SPEED2, IMBALANCE, LOOSE_MOUNT, PHASE_LOSS };
enum Fk { FK_NONE, FK_IMB, FK_MOUNT, FK_PHASE };

static bool machine_running = false;   // boots NOT RUNNING
static int  machine_sp = 0;            // SP_HZ index
static Fk   machine_fk = FK_NONE;

static const char* truth_ctx() {
  return machine_running ? (machine_sp == 0 ? "op_speed_1" : "op_speed_2")
                         : "op_stopped";
}

static const char* truth_name() {
  if (!machine_running) return "op_stopped";
  switch (machine_fk) {
    case FK_IMB:   return "imbalance";
    case FK_MOUNT: return "loose_mount";
    case FK_PHASE: return "phase_loss";
    default: break;
  }
  return truth_ctx();
}

class ScriptedSensor : public ISensorNode {
public:
  void setProfile(const FeatureProfile* p) { prof_ = p; }
  void setSeed(unsigned long s) { lcg_ = s; }

  bool begin() override { step_ = 0; return true; }
  void poll() override { step_++; scenario(); }
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

  // scripted-truth hint for the CI agent = machine truth + operating point
  // (faults are labelled as compound slugs: imbalance@op_speed_1, ...)
  const char* anomalyLabel() {
    const char* t = truth_name();
    if (machine_fk == FK_NONE) return t;
    snprintf(label_buf_, sizeof(label_buf_), "%s@%s", t, truth_ctx());
    return label_buf_;
  }

  // polls so far; lets the host advance time through idle phases
  long polls() const { return step_; }

  // command the machine; the speed itself ramps, the fault overlays
  void setMachine(bool run, int sp, Fk fk) {
    float target = run ? SP_HZ[sp] : 0.0f;
    ramp_rate_ = fabsf(target - f_ref_) / 2.0f;   // ~2 s to the setpoint
    f_ref_ = target;
  }

private:
  // band edges of the real ADXL FFT (vibration_sensor.h)
  static int band_of(float f) {
    if (f <= 7.8f) return 0;
    if (f <= 23.4f) return 1;
    if (f <= 54.7f) return 2;
    return 3;
  }
  void add_tone(float f, float p, float* b) const {
    if (p <= 0.0f || f < 1.6f) return;   // below band 1: no tone
    b[band_of(f)] += p;
  }

  void scenario() {
    // actual speed chases the commanded target at the commanded ramp rate
    if (f_out_ < f_ref_)
      f_out_ += (f_ref_ - f_out_ < ramp_rate_) ? f_ref_ - f_out_ : ramp_rate_;
    else if (f_out_ > f_ref_)
      f_out_ -= (f_out_ - f_ref_ < ramp_rate_) ? f_out_ - f_ref_ : ramp_rate_;
    bool ramping = fabsf(f_ref_ - f_out_) > 0.1f;
    bool accel = f_ref_ > f_out_;
    if (prof_ == &PROF_G100) scenario_g100(ramping, accel);
    else                     scenario_vib();
  }

  void scenario_g100(bool ramping, bool accel) {
    // constant-torque load: current/torque ~flat across speeds. The set
    // frequency register reports the COMMANDED setpoint directly (user
    // fact: no motor feedback needed) - it steps instantly on a setpoint
    // move and reads 0 while stopped, making the drive's speed/stopped
    // conditions cleanly separable. Bus stays charged, output current 0.
    float warm = 1.0f - expf(-step_ / 900.0f);   // gentle thermal drift
    float fset = f_ref_;
    float i = 3.0f + 0.05f * warm;
    float tq = 45.0f + 0.30f * warm;
    float vdc = 585.0f;
    float ni = 0.05f, nvdc = 1.0f, ntq = 1.5f;
    if (ramping) {                                // accel/decel transient
      if (accel) { i += 0.8f; tq += 12.0f; vdc -= 2.5f; }
      else       { i -= 0.4f; tq -= 8.0f; vdc += 2.5f; }
    }
    switch (machine_fk) {
      case FK_IMB: i += 0.05f; tq += 1.2f; break;          // barely notices
      case FK_MOUNT: i += 0.10f; tq += 2.0f; ntq = 4.5f; break;
      case FK_PHASE: i = i * 1.30f; tq -= 12.0f;
                     vdc -= 3.0f; ni = 0.12f; nvdc = 2.5f; break;
      default: break;
    }
    if (f_out_ < 0.5f) {   // output disabled; setpoint register still reads
      i = 0.0f; tq = 0.0f;
      ni = 0.02f; ntq = 0.1f;
    }
    f_[0] = i + noise(lcg_, ni);
    f_[1] = fset + noise(lcg_, 0.01f);
    f_[2] = vdc + noise(lcg_, nvdc);
    f_[3] = fabsf(tq + noise(lcg_, ntq));
  }

  void scenario_vib() {
    // rms grows with the 1x mechanical speed; tones land in the FFT band
    // that contains their frequency, so band content MOVES with speed.
    float f_mech = 0.485f * f_out_;   // 1x (4-pole on a VFD)
    float rms = f_out_ > 0.5f ? 0.005f + 0.0048f * f_mech : 0.004f;
    switch (machine_fk) {
      case FK_IMB: rms *= 1.5f; break;
      case FK_MOUNT: rms *= 1.2f; break;
      case FK_PHASE: rms *= 1.3f; break;
      default: break;
    }
    float r2 = rms * rms;
    float b[4] = {0, 0, 0, 0};
    add_tone(f_mech, 0.55f * r2, b);           // 1x running speed
    add_tone(2 * f_mech, 0.12f * r2, b);       // 2x
    add_tone(3 * f_mech, 0.05f * r2, b);       // 3x
    for (int k = 0; k < 4; k++) b[k] += 0.07f * r2;   // broadband floor
    switch (machine_fk) {
      case FK_IMB: add_tone(f_mech, 0.90f * r2, b); break;      // 1x dominates
      case FK_MOUNT:            // broadband rattle + running-speed harmonics
        for (int k = 0; k < 4; k++) b[k] += 0.30f * r2;
        add_tone(2 * f_mech, 0.30f * r2, b);
        add_tone(3 * f_mech, 0.25f * r2, b);
        add_tone(4 * f_mech, 0.15f * r2, b);
        break;
      case FK_PHASE:                                            // 2x electrical
        add_tone(2.0f * f_out_, 0.85f * r2, b);
        break;
      default: break;
    }
    for (int k = 0; k < 4; k++) b[k] *= (1.0f + noise(lcg_, 0.4f));
    f_[0] = rms * (1.0f + noise(lcg_, 0.2f));
    f_[1] = b[0]; f_[2] = b[1]; f_[3] = b[2]; f_[4] = b[3];
    f_[5] = f_out_ < 0.5f ? 0.8f + noise(lcg_, 1.5f)              // floor
          : machine_fk == FK_PHASE
            ? 2.0f * f_out_ + noise(lcg_, 0.5f)                   // 2x elec
            : f_mech + noise(lcg_, 0.3f);                         // 1x
  }

  const FeatureProfile* prof_ = &PROF_G100;
  unsigned long lcg_ = 12345;
  float f_[8] = {0};
  long step_ = 0;
  char label_buf_[48] = "";   // compound CI truth label
  float f_ref_ = 0.0f;    // commanded speed (Hz), 0 = not running
  float f_out_ = 0.0f;    // actual speed (Hz), ramps toward f_ref_
  float ramp_rate_ = 0.0f;
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
static void set_machine(const char* name);

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
      // truth conditions starting op_ are NORMAL operating points; the
      // rest are faults - the kind rides with the label (B-rule)
      const char* kd = (lb && strncmp(lb, "op_", 3) == 0) ? "normal" : "fault";
      char j[128];
      if (lb) {
        snprintf(j, sizeof(j),
                 "{\"action\":\"label\",\"label\":\"%s\","
                 "\"kind\":\"%s\",\"by\":\"agent\"}", lb, kd);
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
  // (learn on a running motor, exercise stop/start, then three faults)
  if (autoagent_on) {
    static const struct { long at; const char* cmd; } inj[] = {
      {40, "START"}, {160, "STOP"}, {200, "START"},
      {300, "IMBALANCE"}, {330, "NORMAL"},
      {480, "LOOSE_MOUNT"}, {510, "NORMAL"},
      {660, "PHASE_LOSS"}, {690, "NORMAL"},
    };
    static int inj_idx = 0;
    if (inj_idx < 9 && rigs[0].last_step >= inj[inj_idx].at) {
      set_machine(inj[inj_idx].cmd);
      inj_idx++;
    }
  }
  for (int i = 0; i < n_nodes; ++i) {
    if (strcmp(rigs[i].app.phase(), "idle") == 0) {
      // no monitor steps while idle (feed is learning-gated): advance one
      // sensor poll (1 s) so time still flows before START
      long p0 = rigs[i].sensor.polls();
      while (rigs[i].sensor.polls() == p0) rigs[i].app.tick();
    } else {
      long before = rigs[i].app.stepCount();
      while (rigs[i].app.stepCount() == before) rigs[i].app.tick();
    }
    rigs[i].last_step = rigs[i].sensor.polls();   // S-line step = sim time
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

// user commands from the web panel (F protocol); pushes to every sensor
static void set_machine(const char* name) {
  if (strcmp(name, "START") == 0) {
    machine_running = true;
    // learning gate: nodes calibrate on the LIVE running motor, never on
    // a stopped one. Safe to call again: theta-ready nodes just print a
    // hint; the typed console 'reset' stays the factory-reset path.
    for (int i = 0; i < n_nodes; ++i)
      if (strcmp(rigs[i].app.phase(), "idle") == 0)
        rigs[i].app.startLearning();
  } else if (strcmp(name, "STOP") == 0) {
    machine_running = false;
  } else if (strcmp(name, "SPEED1") == 0) {
    machine_sp = 0;
  } else if (strcmp(name, "SPEED2") == 0) {
    machine_sp = 1;
  } else if (strcmp(name, "IMBALANCE") == 0) {
    machine_fk = FK_IMB;
  } else if (strcmp(name, "LOOSE_MOUNT") == 0) {
    machine_fk = FK_MOUNT;
  } else if (strcmp(name, "PHASE_LOSS") == 0) {
    machine_fk = FK_PHASE;
  } else if (strcmp(name, "NORMAL") == 0) {
    machine_fk = FK_NONE;
  } else {
    return;
  }
  for (int i = 0; i < n_nodes; ++i)
    rigs[i].sensor.setMachine(machine_running, machine_sp, machine_fk);
  printf("M %s %s\n", truth_name(), truth_ctx());
  printf("[MACHINE] %s at %s\n", truth_name(), truth_ctx());
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
    // centroid mapped through the sibling's own frozen space; the kind
    // travels with the slug (parsed from the label JSON)
    if (&rigs[j].sensor.profile() == &rigs[idx].sensor.profile()
        && slug[0]) {
      float means[8];
      rigs[idx].app.episodeMeans(means);
      CondKind k = strstr(json, "\"kind\":\"normal\"") ? COND_NORMAL
                                                       : COND_FAULT;
      int s = rigs[j].app.shareLabel(slug, means, k);
      printf("[SHARE] centroid '%s' (%s) -> node %d (lib idx %d)\n",
             slug, k == COND_NORMAL ? "normal" : "fault", j, s);
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
    rigs[i].app.begin(rigs[i].sensor, id);   // nodes idle until START
  }

  printf("META nodes=%d steps=%ld", n_nodes, total);
  for (int i = 0; i < n_nodes; ++i)
    printf(" node%d=%s nf=%d", i,
           rigs[i].sensor.profile().node_type,
           rigs[i].sensor.profile().n_features);
  printf("\nREADY\n");
  printf("M %s %s\n", truth_name(), truth_ctx());   // boot truth: NOT running

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
