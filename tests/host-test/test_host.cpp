// =====================================================================
// test_host.cpp -- host-side simulation of the TinyRL condition monitor.
// Compiles the REAL general-core modules against the REAL TinyRL headers
// with Arduino stubs, then:
//   A) CAL -> theta fit on a synthetic fixed-load induction-motor stream
//      (noise-stationary + slow thermal drift; see sample() rationale)
//   B) 200 quiet steps  -> must NOT alarm
//   C) persistence round-trip (model.bin + net.bin via RAM LittleFS)
//   D) injected anomaly -> MUST alarm within 100 steps
//   E) condition library label -> save -> load -> match
// =====================================================================
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "Arduino.h"
#include "WiFi.h"
#include "tinyrl_monitor.h"
#include "condition_library.h"
#include "episode.h"
#include "persistence.h"
#include "plasticity.hpp"

// ---- deterministic noise ----------------------------------------------
static unsigned long lcg = 12345;
static float noise(float scale) {
  lcg = lcg * 1664525u + 1013904223u;
  float u = (float)(lcg % 10000) / 10000.0f - 0.5f;
  return 2.0f * scale * u;
}

// ---- fake sensor with the SAME profile as the G100 node ----------------
static constexpr FeatureInfo FEATS[4] = {
  {"i", "A"}, {"slip", "Hz"}, {"vdc", "V"}, {"tq", "%"},
};
static const FeatureProfile PROF = {"fake_vfd", 4, FEATS};

// ---- fixed-load induction motor at 1 Hz (fixture rationale 2026-11-13) -
// At a fixed VFD setpoint the 1 Hz stream is noise-stationary with slow
// thermal drift: winding/rotor resistance rise nudges i, slip and the
// torque estimate over the first ~15 min; line-frequency and vibration
// dynamics are invisible at this rate. The earlier 4-min sine waves
// over-stressed the fixture and forced a >=1-period LEARN (probes
// 2026-11-13); removed to match the real machine.
static void sample(float* x, long t, bool anomaly) {
  float warm = 1.0f - expf(-t / 900.0f);   // thermal warm-up, ~15 min constant
  x[0] = 3.0f + 0.05f * warm + noise(0.05f);   // i [A]: +~1.7% on warm-up
  x[1] = 0.25f + 0.008f * warm + noise(0.03f); // slip [Hz]: rotor R rise
  x[2] = 585.0f + noise(1.0f);                 // vdc [V]: regulated bus
  x[3] = 45.0f + 0.30f * warm + noise(1.5f);   // tq [%]: model-estimate drift
  if (anomaly) { x[0] += 1.2f; x[1] += 0.9f; x[2] -= 15.0f; x[3] += 30.0f; }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const bool trace = getenv("TINYRL_TRACE") != nullptr;   // per-step log for figures
  TinyRLConditionMonitor mon;
  mon.begin(PROF);
  mon.startLearning();

  float x[4];
  TinyRLConditionMonitor::StepOut out;

  // plasticity audit state (Dohare 2024; host-test/bench logging only):
  // replica of the monitor's Welford normalizer + ring of recent raw samples
  plast::ZReplica zr;
  plast::RawWindow rawWin;
  auto record = [&](const float* s) {
    if (!mon.thetaReady()) zr.update(s, 4);   // frozen moments after theta fit
    rawWin.push(s, 4);
  };

  // ---- A: CAL + FIT (durations from node_config.h) ---------------------
  long t = 0;
  for (; t < cfg::CAL_STEPS; ++t) { sample(x, t, false); record(x); mon.step(x, out); }
  assert(mon.space().armed());
  printf("A: CAL done, ranges frozen\n");
  for (; t < cfg::CAL_STEPS + cfg::FIT_STEPS; ++t) { sample(x, t, false); record(x); mon.step(x, out); }
  assert(mon.thetaReady());
  assert(!mon.alarmed());
  printf("A: theta=%.3f score=%.3f conf=%.2f state=%s\n",
         mon.theta(), mon.score(), mon.confidence(), mon.stateString());
  plast::audit(mon, zr, rawWin, 4, t);

  // ---- B: 200 quiet steps -> no alarm -----------------------------------
  bool false_alarm = false;
  for (; t < cfg::CAL_STEPS + cfg::FIT_STEPS + 200; ++t) {
    sample(x, t, false);
    record(x);
    mon.step(x, out);
    if (trace)
      printf("TRACE step=%ld tag=quiet score=%.4f theta=%.4f\n",
             (long)mon.steps(), mon.score(), mon.theta());
    if (out.alarm_open) false_alarm = true;
  }
  assert(!false_alarm && !mon.alarmed());
  printf("B: 200 quiet steps OK, score=%.3f\n", mon.score());
  plast::audit(mon, zr, rawWin, 4, t);

  // ---- C: persistence round-trip (calm point, before any alarm) ---------
  assert(persist::modelSave(mon));
  assert(persist::netSave(mon));
  float theta_ref = mon.theta();

  TinyRLConditionMonitor mon2;
  mon2.begin(PROF);
  assert(persist::modelLoad(mon2));
  assert(persist::netLoad(mon2));
  assert(mon2.thetaReady());
  assert(fabsf(mon2.theta() - theta_ref) < 1e-6f);
  // same inputs -> same scores with the restored net. The serialized state
  // has no previous-step tensor, so mon2's first surprise differs from
  // mon's; settle both on quiet steps until the EMA history washes out
  // (0.85^70 ~ 1e-5), then compare. Residual divergence stays bounded by
  // the unsaved optimizer state (traces + leaky max), see net.bin note in
  // ARCHITECTURE.md.
  TinyRLConditionMonitor::StepOut out2;
  for (int k = 0; k < 70; ++k, ++t) {
    sample(x, t, false);
    mon.step(x, out2);
    mon2.step(x, out2);
  }
  printf("C: restored theta=%.3f, score after settle: live=%.4f restored=%.4f\n",
         mon2.theta(), mon.score(), mon2.score());
  assert(fabsf(mon2.score() - mon.score()) < 0.05f * fmaxf(1.0f, mon.score()));

  // ---- D: anomaly -> alarm ----------------------------------------------
  int alarm_at = -1;
  float x_anom[4];
  for (int k = 0; k < 100; ++k, ++t) {
    sample(x, t, true);
    record(x);
    mon.step(x, out);
    if (trace)
      printf("TRACE step=%ld tag=anomaly score=%.4f theta=%.4f%s\n",
             (long)mon.steps(), mon.score(), mon.theta(),
             out.alarm_open ? " event=alarm" : "");
    if (out.alarm_open) { alarm_at = k; memcpy(x_anom, x, sizeof(x)); break; }
  }
  assert(mon.alarmed() && alarm_at >= 0);
  printf("C: ALARM opened after %d anomaly steps, score=%.3f theta=%.3f\n",
         alarm_at, mon.score(), mon.theta());
  plast::audit(mon, zr, rawWin, 4, t);

  // episode summary must be generic and readable
  EpisodeManager ep;
  ConditionLibrary lib;
  lib.begin(4);
  ep.begin(&PROF, &lib, "g100-01",
           [](const char* ev, const char* sev, const char* cid, uint32_t dur,
              const char* sum, const float*) {
             printf("   event: %s sev=%s cond=%s dur=%lus\n%s\n", ev, sev,
                    cid, (unsigned long)dur, sum);
           });
  ep.open(x, mon.space(), (uint32_t)t * 1000);
  char sum[192];
  ep.summary(sum, sizeof(sum), (uint32_t)t * 1000 + 3000);
  assert(strstr(sum, "i=") && strstr(sum, "vdc="));
  printf("E0: generic summary: %s\n", sum);

  // ---- E: condition library label -> save -> load -> match --------------
  float mean[4];
  ep.means(mean);
  int idx = lib.label("belt_slip", mean, mon.space(), "agent-01");
  assert(idx == 0);
  assert(lib.save(cfg::COND_FILE));
  ConditionLibrary lib2;
  lib2.begin(4);
  assert(lib2.load(cfg::COND_FILE));
  assert(lib2.size() == 1);
  int m = lib2.match(x_anom, mon2.space());
  assert(m == 0);
  sample(x, t, false);                             // fresh normal sample
  int m_norm = lib2.match(x, mon2.space());        // normal sample: no match
  printf("E: labeled 'belt_slip' -> persisted -> anomaly matched idx=%d, "
         "normal matched idx=%d\n", m, m_norm);
  assert(m_norm == -1);

  // ---- F: plasticity counters + optional long run (Dohare 2024, RQ2) -----
  // Checkpoint audits above log dead-unit fraction / mean |w| / effective
  // rank at fit, quiet and alarm points on every run. TINYRL_LONGRUN=<steps>
  // (default 100000) extends the stream with regime shifts (machine life:
  // slow load drift) and logs the counters at a fixed interval. Logging
  // only: nothing asserted, nothing published, nothing on the node.
  long lr_steps = 0;
  if (const char* e = getenv("TINYRL_LONGRUN")) lr_steps = atol(e);
  if (lr_steps > 0) {
    const long report = lr_steps > 1000000 ? lr_steps / 20 : 5000;
    const long regime = lr_steps / 4;              // 4 regimes
    float off[4] = {0, 0, 0, 0};
    printf("F: long-run plasticity bench: %ld steps (report every %ld)\n",
           lr_steps, report);
    for (long k = 1; k <= lr_steps; ++k, ++t) {
      if (regime > 0 && k % regime == 0) {         // regime shift
        off[0] += 0.4f; off[1] += 0.15f; off[3] += 4.0f;
        printf("F: regime shift at step %lu (offsets %.2f/%.2f/%.2f)\n",
               (unsigned long)mon.steps(), off[0], off[1], off[3]);
      }
      sample(x, t, false);
      x[0] += off[0]; x[1] += off[1]; x[3] += off[3];
      record(x);                                   // moments frozen; window only
      mon.step(x, out);
      if (k % report == 0) plast::audit(mon, zr, rawWin, 4, t);
    }
    plast::audit(mon, zr, rawWin, 4, t);
    printf("F: long-run done at step %lu\n", (unsigned long)mon.steps());
  } else {
    plast::audit(mon, zr, rawWin, 4, t);           // final default-path snapshot
  }

  printf("\nALL HOST TESTS PASSED\n");
  return 0;
}
