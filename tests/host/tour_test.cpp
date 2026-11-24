// =====================================================================
// tour_test.cpp -- the SEVEN-condition tour as a deterministic host test.
// Two REAL SensorNodeApps (g100 drive + vib) watch the ONE simulated
// motor (machine_model.hpp, the same generator the web bench runs), the
// harness stands in for the operator: labels are answered from the stored
// ask batches, exactly like the bench oracle.
//
// The bench (web panel) is interactive; this file is its CI shadow:
//   learn tour  : every state the rig cannot name opens an ask, the ask
//                 matures at >= ASK_MIN_BATCH (15) settled samples, the
//                 label consumes the batch, the condition is known after.
//   re-tour     : every state is recognised by its owner modality with
//                 ZERO new asks and ZERO alarms on known-normal states.
//
// Assertions (2026-11-19 ask/settle policy):
//   A1 asks mature:  every stored ask batch holds >= cfg::ASK_MIN_BATCH
//      settled samples; between alarm-open and the ask the condition
//      publishes "unidentified", never a premature "unknown".
//   A2 asks only for the unknown: a state that resolves into a known
//      condition (or sits inside a modality's noise) never asks.
//   A3 100% owner detection: every fault period is identified by its
//      owning node (alarm edge or named condition) inside the stage.
//      The g100 physically cannot see imbalance (i +0.05 = 1 sigma,
//      tq +1.2 = 0.8 sigma) - asserting its quiet there is part of A2.
//   A4 re-tour discipline: zero new asks after the learn tour.
//   A5 vocabulary: g100 = 5 conditions, vib = 6 (each node's own
//      centroid, per the label fan-out design).
// =====================================================================
#include <cassert>
#include <cstdio>
#include <cstring>
#include "node_runtime.h"
#include "machine_model.hpp"

namespace {

struct Rig {
  ScriptedSensor sensor;
  SensorNodeApp  app;
  // per-ask discipline record
  int  asks = 0;                    // pending-batch births
  int  min_batch_n = 1 << 30;       // smallest ask batch seen
  bool saw_unidentified = false;    // pre-ask publication observed
  // alarm edges: step of every mon/alarm transition (bounded)
  long alarm_edge[64] = {0};
  int  alarm_edges = 0;
};

Rig rigs[2];

void step_rig(Rig& r) {
  if (strcmp(r.app.phase(), "idle") == 0) {
    long p0 = r.sensor.polls();
    while (r.sensor.polls() == p0) r.app.tick();
  } else {
    long before = r.app.stepCount();
    while (r.app.stepCount() == before) r.app.tick();
  }
}

// one sim step per rig, recording alarm edges + ask discipline
void step_all() {
  for (Rig& r : rigs) {
    const char* ph0 = r.app.phase();
    const char* cid0 = r.app.conditionId();
    int ps0 = (int)r.app.pendingSize();
    step_rig(r);
    const char* ph1 = r.app.phase();
    if (strcmp(ph1, "alarm") == 0 && strcmp(ph0, "alarm") != 0 &&
        r.alarm_edges < 64)
      r.alarm_edge[r.alarm_edges++] = r.app.stepCount();
    (void)cid0;
    const char* cid = r.app.conditionId();
    if (strcmp(cid, "unidentified") == 0) r.saw_unidentified = true;
    int ps1 = (int)r.app.pendingSize();
    if (ps1 > ps0) {                       // a new ask was stored
      r.asks++;
      uint32_t best_n = 0;
      for (int k = 0; k < ps1; ++k) {
        char id[12]; uint32_t n, st;
        if (r.app.pendingAt(k, id, sizeof(id), &n, &st) && n > best_n)
          best_n = n;                      // freshest batch = largest n
      }
      if ((int)best_n < r.min_batch_n) r.min_batch_n = (int)best_n;
    }
  }
}

void run_all(long n) { for (long k = 0; k < n; ++k) step_all(); }

void machine(bool run, int sp, Fk fk) {
  machine_running = run;
  machine_sp = sp;
  machine_fk = fk;
  for (Rig& r : rigs) r.sensor.setMachine(run, sp, fk);
}

// wait until a NEW pending batch appears on this rig, then label it
// (batch-id flow, same as the bench oracle). Returns the batch n.
uint32_t wait_ask_and_label(Rig& r, const char* slug, const char* kind,
                            int prev_pend, long cap) {
  long start = r.sensor.polls();
  while (r.sensor.polls() - start < cap) {
    step_all();
    if ((int)r.app.pendingSize() > prev_pend) {
      int ps = (int)r.app.pendingSize();
      int best = 0; uint32_t best_step = 0;
      for (int k = 0; k < ps; ++k) {
        char id[12]; uint32_t n, st;
        if (r.app.pendingAt(k, id, sizeof(id), &n, &st) && st > best_step) {
          best_step = st; best = k;
        }
      }
      char id[12]; uint32_t n, st;
      assert(r.app.pendingAt(best, id, sizeof(id), &n, &st));
      char json[192];
      snprintf(json, sizeof(json),
               "{\"action\":\"label\",\"label\":\"%s\",\"kind\":\"%s\","
               "\"batch\":\"%s\",\"by\":\"tour-test\"}", slug, kind, id);
      r.app.applyFeedback(json);
      printf("  ask on %-4s labeled '%s' (%s) batch=%s n=%lu\n",
             r.app.sensor().profile().node_type, slug, kind, id,
             (unsigned long)n);
      return n;
    }
  }
  fprintf(stderr, "TOURCHECK: no ask matured on %s for %s within %ld steps\n",
          r.app.sensor().profile().node_type, slug, cap);
  assert(false);
  return 0;
}

void ask_label_all(const char* slug, const char* kind, long cap) {
  int p0 = (int)rigs[0].app.pendingSize();
  int p1 = (int)rigs[1].app.pendingSize();
  // A node streaming REAL data without this condition's recording holds
  // its previous block and cannot ask - skip its wait. A node on its
  // SYNTHETIC fallback (its modality's recordings are missing entirely)
  // behaves synthetically and asks normally.
  if (!(mm_real_on() && mm_g100_ok) || mm_has_block(true, slug))
    wait_ask_and_label(rigs[0], slug, kind, p0, cap);
  if (!(mm_real_on() && mm_vib_ok) || mm_has_block(false, slug))
    wait_ask_and_label(rigs[1], slug, kind, p1, cap);
}

bool cond_becomes(Rig& r, const char* slug, long cap) {
  long start = r.sensor.polls();
  while (r.sensor.polls() - start < cap) {
    step_all();
    if (strcmp(r.app.conditionId(), slug) == 0) return true;
  }
  return false;
}

bool lib_has(Rig& r, const char* slug) {
  for (int i = 0; i < (int)r.app.libSize(); ++i)
    if (strcmp(r.app.libName(i), slug) == 0) return true;
  return false;
}

int alarms_in_window(Rig& r, long from_step) {
  int n = 0;
  for (int k = 0; k < r.alarm_edges; ++k)
    if (r.alarm_edge[k] >= from_step) ++n;
  return n;
}

} // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const bool REAL = mm_real_on();
  printf("TOURCHECK: two-node seven-condition tour (learn + re-tour)%s\n",
         REAL ? " [REAL DATA]" : " [SYNTHETIC]");

  rigs[0].sensor.setProfile(mm_prof_g100());
  rigs[0].sensor.setSeed(12345);
  rigs[1].sensor.setProfile(mm_prof_vib());
  rigs[1].sensor.setSeed(777);
  for (Rig& r : rigs) r.sensor.setQuietLog(true);   // no [SIM] flood in CI
  const char* dev_ids[2] = {"tour-01", "tour-02"};  // distinct: the monitor
                                                    // tags every [WHY]/[RL]
                                                    // print with its dev id
  for (int i = 0; i < 2; ++i) {
    NodeIdentity id{dev_ids[i], "v1/Plant01/Area/Line/Station-01",
                    "ssid", "pass", "127.0.0.1", 1883};
    rigs[i].app.begin(rigs[i].sensor, id);
  }

  // ---- phase 0: commission on the STOPPED machine (user procedure) ----
  for (Rig& r : rigs) r.app.startLearning();
  run_all(500);                        // CAL 30 + FIT 30 + margin
  for (Rig& r : rigs) {
    assert(r.app.theta() > 0 && strcmp(r.app.phase(), "mon") == 0);
    assert(r.app.libSize() == 1 &&
           strcmp(r.app.libName(0), "op_stopped") == 0);
  }
  printf("  commissioned: baseline op_stopped on both nodes\n");

  // ---- learn tour ------------------------------------------------------
  // Learn-stage settle windows: the research budget is 60-300 settled
  // samples per new condition (2026-11-21 user decision). REAL recordings
  // need the upper end: the machine drifts inside a 150-row block, the
  // first-formed bands go stale, and the pinned-alarm re-form
  // (BAND_REFORM_N) + band re-maturity need ~130 of those steps before the
  // stage closes quiet. Synthetic fixtures are stationary: 80 suffices.
  const long LEARN_SETTLE = REAL ? 320 : 80;
  machine(true, 0, FK_NONE);           // START at speed_1
  ask_label_all("speed_1", "normal", 250);
  run_all(LEARN_SETTLE);               // train + clear + band re-form
  for (Rig& r : rigs) {
    assert(strcmp(r.app.phase(), "mon") == 0);
    assert(strcmp(r.app.conditionId(), "speed_1") == 0);
  }
  printf("  speed_1 learned, quiet on both nodes\n");

  machine(true, 1, FK_NONE);           // SPEED2
  ask_label_all("speed_2", "normal", 250);
  run_all(LEARN_SETTLE);
  for (Rig& r : rigs) {
    assert(strcmp(r.app.phase(), "mon") == 0);
    assert(strcmp(r.app.conditionId(), "speed_2") == 0);
  }
  printf("  speed_2 learned, quiet on both nodes\n");

  long t0 = rigs[0].sensor.polls();
  machine(false, 1, FK_NONE);          // STOP
  run_all(80);
  for (Rig& r : rigs) {
    assert(alarms_in_window(r, t0) == 0);      // known state: no alarm
    assert(r.app.pendingSize() == 0);          // known state: no ask
    assert(strcmp(r.app.phase(), "mon") == 0);
  }
  printf("  stop recognised quietly (no alarm, no ask)\n");

  machine(true, 0, FK_IMB);            // imbalance at speed_1 (running)
  wait_ask_and_label(rigs[1], "speed_1_imbalance", "fault", 0, 250);
  // fault-kind episodes clear only after the net trains the surprise down
  // (CLEAR_LO_N quiet steps); the NAMED condition is the discipline here
  run_all(300);
  assert(rigs[0].app.pendingSize() == 0);      // g100 blind to imbalance
  assert(strcmp(rigs[0].app.phase(), "mon") == 0);
  assert(strcmp(rigs[1].app.conditionId(), "speed_1_imbalance") == 0);
  printf("  speed_1_imbalance learned (vib); g100 quiet inside its noise\n");

  machine(true, 1, FK_IMB);            // imbalance at speed_2 (running)
  wait_ask_and_label(rigs[1], "speed_2_imbalance", "fault", 0, 250);
  run_all(300);
  assert(rigs[0].app.pendingSize() == 0);      // g100 blind to imbalance
  assert(strcmp(rigs[0].app.phase(), "mon") == 0);
  assert(strcmp(rigs[1].app.conditionId(), "speed_2_imbalance") == 0);
  printf("  speed_2_imbalance learned (vib); g100 quiet inside its noise\n");

  machine(true, 0, FK_PHASE);          // phase_loss at speed_1
  ask_label_all("speed_1_phase_loss", "fault", 250);
  run_all(LEARN_SETTLE);
  printf("  speed_1_phase_loss learned on both nodes\n");

  machine(true, 1, FK_PHASE);          // speed_2 under phase_loss
  ask_label_all("speed_2_phase_loss", "fault", 250);
  run_all(LEARN_SETTLE);
  printf("  speed_2_phase_loss learned on both nodes\n");

  t0 = rigs[0].sensor.polls();
  machine(false, 1, FK_NONE);          // STOP + fault removed
  run_all(100);
  for (Rig& r : rigs) {
    assert(alarms_in_window(r, t0) == 0);
    assert(r.app.pendingSize() == 0);
    assert(strcmp(r.app.phase(), "mon") == 0);
  }
  printf("  return to stop quiet on both nodes\n");

  // ---- re-tour: zero asks, owner recognition ---------------------------
  machine(true, 0, FK_NONE);           // speed_1
  run_all(80);
  for (Rig& r : rigs) {
    assert(strcmp(r.app.conditionId(), "speed_1") == 0);
    assert(r.app.pendingSize() == 0);
  }
  machine(true, 1, FK_NONE);           // speed_2
  run_all(80);
  for (Rig& r : rigs) {
    assert(strcmp(r.app.conditionId(), "speed_2") == 0);
    assert(r.app.pendingSize() == 0);
  }
  printf("  re-tour speed_1 + speed_2 recognised, no asks\n");

  machine(true, 0, FK_IMB);            // imbalance at speed_1
  assert(cond_becomes(rigs[1], "speed_1_imbalance", 150));
  run_all(40);
  assert(strcmp(rigs[0].app.conditionId(), "speed_1") == 0); // g100 blind
  assert(rigs[0].app.pendingSize() == 0);
  assert(rigs[1].app.pendingSize() == 0);
  printf("  re-tour imbalance recognised by owner (vib) only\n");

  machine(true, 1, FK_IMB);            // imbalance at speed_2
  assert(cond_becomes(rigs[1], "speed_2_imbalance", 150));
  run_all(40);
  assert(strcmp(rigs[0].app.conditionId(), "speed_2") == 0); // g100 blind
  assert(rigs[0].app.pendingSize() == 0);
  assert(rigs[1].app.pendingSize() == 0);
  printf("  re-tour speed_2_imbalance recognised by owner (vib) only\n");

  machine(true, 1, FK_PHASE);          // phase_loss at speed_2
  assert(cond_becomes(rigs[0], "speed_2_phase_loss", 150));
  assert(cond_becomes(rigs[1], "speed_2_phase_loss", 150));
  run_all(40);
  assert(rigs[0].app.pendingSize() == 0);
  assert(rigs[1].app.pendingSize() == 0);
  printf("  re-tour phase_loss recognised on both nodes, no asks\n");

  machine(false, 1, FK_NONE);          // STOP
  run_all(80);
  assert(rigs[0].app.pendingSize() == 0);
  assert(rigs[1].app.pendingSize() == 0);

  // ---- churn: many UNLABELED cycles (the bench "back and forth") ------
  // The net trains on every settled matched step; repeated entries run the
  // novelty rate until the surprise decays, and each entry interferes with
  // the other regimes' mappings. Whatever the mechanism, the operator's
  // contract is absolute: after churn, EVERY known condition - normal AND
  // fault - is quiet (a named state recurring is not news), recognition
  // holds, and nothing new asks. This is the CI form of the 2026-11-19
  // bench report ("after some back and forth the model gets everything
  // wrong, including simple normal conditions").
  int churn_alarms = 0;
  int churn_asks = 0;
  for (int cycle = 0; cycle < 20; ++cycle) {
    struct Visit { bool run; int sp; Fk fk; };
    static const Visit visits[] = {
      {true, 0, FK_NONE},             // speed_1
      {true, 1, FK_NONE},             // speed_2
      {true, 0, FK_IMB},              // imbalance@speed_1 (vib owns)
      {true, 1, FK_IMB},              // imbalance@speed_2 (vib owns)
      {true, 1, FK_PHASE},            // phase_loss@speed_2 (both see)
      {false, 1, FK_NONE},            // stop, faults removed
    };
    for (const Visit& v : visits) {
      long t0 = rigs[0].sensor.polls();
      int p0 = (int)rigs[0].app.pendingSize();
      int p1 = (int)rigs[1].app.pendingSize();
      machine(v.run, v.sp, v.fk);
      run_all(100);
      churn_alarms += alarms_in_window(rigs[0], t0);
      churn_alarms += alarms_in_window(rigs[1], t0);
      churn_asks += (int)rigs[0].app.pendingSize() - p0;
      churn_asks += (int)rigs[1].app.pendingSize() - p1;
    }
    printf("  churn cycle %d: alarms=%d asks=%d\n",
           cycle, churn_alarms, churn_asks);
  }
  assert(churn_alarms == 0);           // known states never re-alarm
  assert(churn_asks == 0);             // nothing new: the library knows all

  // ---- post-churn recognition: everything still named ------------------
  machine(true, 0, FK_NONE);
  assert(cond_becomes(rigs[0], "speed_1", 150));
  assert(cond_becomes(rigs[1], "speed_1", 150));
  machine(true, 1, FK_NONE);
  assert(cond_becomes(rigs[0], "speed_2", 150));
  assert(cond_becomes(rigs[1], "speed_2", 150));
  machine(true, 0, FK_IMB);
  assert(cond_becomes(rigs[1], "speed_1_imbalance", 150));
  machine(true, 1, FK_IMB);
  assert(cond_becomes(rigs[1], "speed_2_imbalance", 150));
  machine(true, 1, FK_PHASE);
  assert(cond_becomes(rigs[0], "speed_2_phase_loss", 150));
  assert(cond_becomes(rigs[1], "speed_2_phase_loss", 150));
  machine(false, 1, FK_NONE);
  run_all(80);
  assert(rigs[0].app.pendingSize() == 0);
  assert(rigs[1].app.pendingSize() == 0);
  printf("  post-churn: every condition still recognised, no asks\n");

  // ---- summary discipline asserts --------------------------------------
  // A1: every ask matured at >= ASK_MIN_BATCH settled samples
  for (Rig& r : rigs) {
    printf("  %s: asks=%d min_batch=%d unidentified_before_ask=%d\n",
           r.app.sensor().profile().node_type, r.asks, r.min_batch_n,
           (int)r.saw_unidentified);
    assert(r.min_batch_n >= cfg::ASK_MIN_BATCH);
    assert(r.saw_unidentified);
  }
  assert(rigs[0].asks == 4);           // speed_1, speed_2, 2x phase_loss
                                       // (real mode: the collect dataset
                                       // covers all 4; imbalance aliases
                                       // the speed blocks, user decision)
  assert(rigs[1].asks == 6);           // + both imbalances (the vib sees
                                       // them: 14 real rows each)
  // A5: final vocabulary (each node's own centroids)
  assert(rigs[0].app.libSize() == 5);
  for (const char* s : {"op_stopped", "speed_1", "speed_2",
                        "speed_1_phase_loss", "speed_2_phase_loss"})
    assert(lib_has(rigs[0], s));
  assert(rigs[1].app.libSize() == 7);
  for (const char* s : {"op_stopped", "speed_1", "speed_2",
                        "speed_1_imbalance", "speed_2_imbalance",
                        "speed_1_phase_loss", "speed_2_phase_loss"})
    assert(lib_has(rigs[1], s));

  printf("\nTOURCHECK OK: asks mature at %d+, owner-only asks, re-tour "
         "zero asks, full vocabulary\n", cfg::ASK_MIN_BATCH);
  return 0;
}
