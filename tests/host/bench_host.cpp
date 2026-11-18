// =====================================================================
// bench_host.cpp -- multi-node host run for the web bench.
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
// Label fan-out ("the nodes work together"):
//   - a user or agent label is applied to the asking node and SHARED:
//     every node of the same machine with an open unknown ask (the
//     same-timeframe event) gets the same slug, each building its own
//     centroid from its own episode means; batch ids are per node and
//     never cross nodes.
//
// Stdin protocol (the web server is the pacing brain):
//   s                       advance every node by ONE monitor step (1 s)
//   F <CMD>                 drive the machine: START|STOP|SPEED1|SPEED2|
//                           IMBALANCE|PHASE_LOSS|NORMAL
//   L <idx> <slug> <json>   applyFeedback(json) on node idx + sharing
//   q                       quit
// Per monitor step: one state line PER node:
//   S <node> <step> <phase> <score> <theta> <ep_open> <cond> <lib> f0 ...
// plus M <state> whenever the machine state changes. Event lines:
// [RL]/[EP]/[FB]/[LIB]/[SHARE]/[MACHINE].
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
//   faults:            imbalance (mechanical), phase_loss (VFD -> motor)
//                      (user decision 2026-11-15: two fault types)
// Fault asks are labelled WITH their operating point (compound slug,
// e.g. imbalance@op_speed_1): faults under different setpoints are
// different library conditions.
// Start/stop and setpoint moves ramp the ACTUAL speed over ~2 s (VFD
// accel/decel). The set-frequency register reports the COMMANDED setpoint
// directly (user fact: no motor feedback needed) - it steps instantly and
// reads 0 while stopped. Drive slip is gone from the feature set.
static constexpr float SP_HZ[2] = {5.0f, 20.0f};
enum Ev { STOPPED, SPEED1, SPEED2, IMBALANCE, PHASE_LOSS };
enum Fk { FK_NONE, FK_IMB, FK_PHASE };

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
    // condition vocabulary (user decision 2026-11-15): normals are
    // op_stopped / speed_1 / speed_2; faults compound as
    // speed_1_imbalance etc. (context comes from the running setpoint)
    const char* t = truth_name();
    if (machine_fk == FK_NONE) {
      if (strncmp(t, "op_", 3) == 0) return t;    // op_stopped
      snprintf(label_buf_, sizeof(label_buf_), "%s", t + 3);
      return label_buf_;                          // speed_1 / speed_2
    }
    snprintf(label_buf_, sizeof(label_buf_), "%s_%s", truth_ctx() + 3, t);
    return label_buf_;                            // speed_1_imbalance ...
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
  // scripted-agent state (CI mode only)
  int  ask_age = 0;
  bool asked = false;
  char pend_sig[256] = "";     // last reported pending state (PEND lines)
};

static Rig rigs[8];
static int n_nodes = 2;

static void print_state();
static void set_machine(const char* name);
static void learn_all();
static void dump_libs();

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
      // the kind rides with the label (B-rule): normals are the three
      // operating points, everything else is a fault
      const char* kd = (lb && (strcmp(lb, "op_stopped") == 0
                               || strcmp(lb, "speed_1") == 0
                               || strcmp(lb, "speed_2") == 0))
                           ? "normal" : "fault";
      char j[160];
      if (lb) {
        // the agent labels the STORED batch (2026-11-15 workflow), not
        // the live episode, so the pending store stays consistent
        char bid[12] = "";
        if (r.app.pendingSize() > 0)
          r.app.pendingAt(0, bid, sizeof(bid), nullptr, nullptr);
        snprintf(j, sizeof(j),
                 "{\"action\":\"label\",\"label\":\"%s\","
                 "\"kind\":\"%s\",\"by\":\"agent\",\"batch\":\"%s\"}",
                 lb, kd, bid);
      } else {
        snprintf(j, sizeof(j), "{\"action\":\"normal\",\"by\":\"agent\"}");
      }
      r.app.applyFeedback(j);
      r.asked = true;
      // CI fan-out: a node of the same machine with an open unknown ask
      // gets the same slug, but ITS OWN batch id (ids are per node -
      // never cross nodes)
      for (int k = 0; k < n_nodes; ++k) {
        if (k == idx) continue;
        if (rigs[k].app.episodeOpen()
            && strcmp(rigs[k].app.conditionId(), "unknown") == 0) {
          char bid2[12] = "";
          if (rigs[k].app.pendingSize() > 0)
            rigs[k].app.pendingAt(0, bid2, sizeof(bid2), nullptr, nullptr);
          char j2[160];
          snprintf(j2, sizeof(j2),
                   "{\"action\":\"label\",\"label\":\"%s\","
                   "\"kind\":\"%s\",\"by\":\"agent\",\"batch\":\"%s\"}",
                   lb, kd, bid2);
          rigs[k].app.applyFeedback(j2);
          printf("[SHARE] '%s' -> node %d (same-machine ask)\n", lb, k);
        }
      }
    } else {
      r.ask_age++;
    }
  } else {
    r.ask_age = 0;
  }
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
  if (autoagent_on) {
    // CI self-injection: the timetable stands in for a human at the panel
    // (learn on the STOPPED machine, then exercise stop/start + faults)
    static const struct { long at; const char* cmd; } inj[] = {
      {5, "LEARN"}, {120, "START"}, {200, "STOP"}, {240, "START"},
      {300, "IMBALANCE"}, {400, "NORMAL"},
      {500, "PHASE_LOSS"}, {600, "NORMAL"},
    };
    static int inj_idx = 0;
    if (inj_idx < (int)(sizeof(inj) / sizeof(inj[0]))
        && rigs[0].last_step >= inj[inj_idx].at) {
      if (strcmp(inj[inj_idx].cmd, "LEARN") == 0) learn_all();
      else set_machine(inj[inj_idx].cmd);
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
    // PEND on ANY change (count, per-batch step or n): the driver's
    // oracle labels by accumulated sample count, so growth must surface
    char sig[256];
    size_t o = 0;
    int pc = (int)rigs[i].app.pendingSize();
    o += snprintf(sig + o, sizeof(sig) - o, "%d", pc);
    for (int k = 0; k < pc; ++k) {
      char id[12];
      uint32_t pn, ps;
      if (rigs[i].app.pendingAt(k, id, sizeof(id), &pn, &ps))
        o += snprintf(sig + o, sizeof(sig) - o, "|%s@%lu@%lu",
                      id, (unsigned long)ps, (unsigned long)pn);
    }
    if (strcmp(sig, rigs[i].pend_sig) != 0) {
      printf("PEND %d %s\n", i, sig);
      snprintf(rigs[i].pend_sig, sizeof(rigs[i].pend_sig), "%s", sig);
    }
  }
}

// learn command: commission on the STOPPED machine (2026-11-15)
static void learn_all() {
  for (int i = 0; i < n_nodes; ++i)
    if (strcmp(rigs[i].app.phase(), "idle") == 0)
      rigs[i].app.startLearning();
  printf("[MACHINE] learn requested (machine %s at %s)\n",
         truth_name(), truth_ctx());
}

// library dump for the report figures (centroid lines in the condition
// space); 'K' on demand, also printed after DONE. LIBU carries each
// condition's own matching units so the diag can check the radii.
static void dump_libs() {
  for (int i = 0; i < n_nodes; ++i) {
    for (int k = 0; k < (int)rigs[i].app.libSize(); ++k) {
      float c[8];
      CondKind kind;
      if (!rigs[i].app.libCentroid(k, c, &kind)) continue;
      printf("LIBC %d %s %s", i, rigs[i].app.libName(k),
             kind == COND_NORMAL ? "normal" : "fault");
      for (int f = 0; f < rigs[i].sensor.profile().n_features; ++f)
        printf(" %.4f", c[f]);
      printf("\n");
      printf("LIBU %d %s", i, rigs[i].app.libName(k));
      for (int f = 0; f < rigs[i].sensor.profile().n_features; ++f)
        printf(" %.4f", rigs[i].app.libUnit(k, f));
      printf("\n");
    }
  }
}

// user commands from the web panel (F protocol); pushes to every sensor
static void set_machine(const char* name) {
  if (strcmp(name, "START") == 0) {
    machine_running = true;
    // learning gate REMOVED (2026-11-15): commissioning happens on the
    // STOPPED machine via 'G' (learn_all), so the baseline condition is
    // the stopped state (user decision)
  } else if (strcmp(name, "STOP") == 0) {
    machine_running = false;
  } else if (strcmp(name, "SPEED1") == 0) {
    machine_sp = 0;
  } else if (strcmp(name, "SPEED2") == 0) {
    machine_sp = 1;
  } else if (strcmp(name, "IMBALANCE") == 0) {
    machine_fk = FK_IMB;
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
  // only LABEL actions fan out to the machine's other nodes;
  // discard/batch-foreign commands must never become labels (a '-'
  // slug would learn garbage)
  bool is_label = strstr(json, "\"action\":\"label\"") != nullptr;
  if (!open || !is_label) { print_state(); return; }
  for (int j = 0; j < n_nodes; ++j) {
    if (j == idx) continue;
    // a node of the same machine stuck in the SAME unknown ask (the
    // same-timeframe event) gets the slug applied to its own episode,
    // WITHOUT the asker's batch id: ids are per node, and each node
    // learns from its own means
    if (rigs[j].app.episodeOpen()
        && strcmp(rigs[j].app.conditionId(), "unknown") == 0) {
      CondKind k = strstr(json, "\"kind\":\"normal\"") ? COND_NORMAL
                                                       : COND_FAULT;
      char j2[128];
      snprintf(j2, sizeof(j2),
               "{\"action\":\"label\",\"label\":\"%s\",\"kind\":\"%s\","
               "\"by\":\"user\"}",
               slug, k == COND_NORMAL ? "normal" : "fault");
      rigs[j].app.applyFeedback(j2);
      printf("[SHARE] '%s' -> node %d (same-machine ask)\n", slug, j);
    }
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
    } else if (line[0] == 'G') {
      learn_all();
    } else if (line[0] == 'K') {
      dump_libs();
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
    printf("DONE node=%d steps=%ld lib=%u pend=%u\n", i, rigs[i].last_step,
           rigs[i].app.libSize(), (unsigned)rigs[i].app.pendingSize());
  // harness-side flash-state report: the stub stores byte-identical
  // content to what LittleFS persists on ESP32-S3. NOTE: with n_nodes > 1
  // the stub namespace is shared (last writer wins); single-node runs
  // report exact per-node state.
  for (int i = 0; i < n_nodes; ++i)
    printf("[FLASH] node=%d model.bin=%zu net.bin=%zu "
           "conditions.jsonl=%zu pending.jsonl=%zu\n", i,
           LittleFS.fileSize("/model.bin"), LittleFS.fileSize("/net.bin"),
           LittleFS.fileSize("/conditions.jsonl"),
           LittleFS.fileSize("/pending.jsonl"));
  dump_libs();
  return 0;
}
