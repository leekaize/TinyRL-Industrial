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
//   S <node> <step> <phase> <score> <theta> <gate> <ragg> <gate_r> <acc>
//     <ep_open> <cond> <lib> f0 ...
// (gate/ragg/gate_r/acc = the per-cond era evidence triple + accumulator;
// -1/-1 for gate/gate_r in the z era). plus M <state> whenever the
// machine state changes. Event lines:
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
#include "machine_model.hpp"


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
    printf("S %d %ld %s %.4f %.4f %.4f %.4f %.4f %.4f %d %s %u",
           i, rigs[i].last_step, rigs[i].app.phase(), rigs[i].app.score(),
           rigs[i].app.theta(), rigs[i].app.gate(), rigs[i].app.ragg(),
           rigs[i].app.gateR(), rigs[i].app.acc(),
           rigs[i].app.episodeOpen() ? 1 : 0,
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
      // LIBX: per-condition score threshold + the feature anchors, so the
      // plots can draw per-condition bands and the true anchored
      // rel(f, x) = log2((x+A)/(mu+A)) condition space
      printf("LIBX %d %s %.4f", i, rigs[i].app.libName(k),
             rigs[i].app.libTheta(k));
      for (int f = 0; f < rigs[i].sensor.profile().n_features; ++f)
        printf(" %.5g", rigs[i].app.fsAnchor(f));
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
  // the M line carries the EXACT step the truth applies from (the next
  // step to run): app.py used to stamp marks with the max-step-at-pump,
  // which lagged the real application point by up to ~10 steps and made
  // correct detections count as pre-inject (bench 2026-11-19)
  printf("M %ld %s %s\n", rigs[0].last_step + 1, truth_name(), truth_ctx());
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
        (n_nodes == 1 && p && strcmp(p, "vib") == 0) ? mm_prof_vib()
        : (n_nodes == 1 && p && strcmp(p, "g100") == 0) ? mm_prof_g100()
        : (i % 2 == 0 ? mm_prof_g100() : mm_prof_vib());
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
  for (int i = 0; i < n_nodes; ++i) {
    const FeatureProfile& pr = rigs[i].sensor.profile();
    printf(" node%d=%s nf=%d", i, pr.node_type, pr.n_features);
    // the REAL profile's names travel with META: the harness-side
    // FEATURES table holds the synthetic sets and cannot know which
    // modality fell back (2026-11-22c real-data default)
    printf(" feats%d=", i);
    for (int f = 0; f < pr.n_features; ++f)
      printf("%s%s", f ? "," : "", pr.features[f].name);
    printf(" units%d=", i);
    for (int f = 0; f < pr.n_features; ++f)
      printf("%s%s", f ? "," : "", pr.features[f].unit);
  }
  printf("\n");
  if (mm_real_on())
    printf("REALMODE on (default): nodes stream the MODBUS collect"
           " campaign; a modality without a trusted capture falls back to"
           " its synthetic generator (g100 CSV, vib CSV or"
           " TINYRL_VIB_SYNTH=1); TINYRL_SYNTH=1 forces synthetic"
           " everywhere\n");
  printf("READY\n");
  printf("M 0 %s %s\n", truth_name(), truth_ctx());   // boot truth: NOT running

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
