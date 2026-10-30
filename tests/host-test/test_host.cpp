// =====================================================================
// test_host.cpp -- host-side simulation of the TinyRL condition monitor.
// Compiles the REAL general-core modules against the REAL TinyRL headers
// with Arduino stubs, then:
//   A) CAL -> theta fit on a synthetic "normal" VFD-like stream
//   B) 200 quiet steps  -> must NOT alarm
//   C) injected anomaly -> MUST alarm within 100 steps
//   D) persistence round-trip (model.bin + net.bin via RAM LittleFS)
//   E) condition library label -> save -> load -> match
// =====================================================================
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
#include "Arduino.h"
#include "WiFi.h"
#include "tinyrl_monitor.h"
#include "condition_library.h"
#include "episode.h"
#include "persistence.h"

// ---- deterministic noise ----------------------------------------------
static unsigned long lcg = 12345;
static float noise(float scale) {
  lcg = lcg * 1664525u + 1013904223u;
  float u = (float)(lcg % 10000) / 10000.0f - 0.5f;
  return 2.0f * scale * u;
}
static float wave(float t, float period, float amp) {
  return amp * sinf(t / period);
}

// ---- fake sensor with the SAME profile as the G100 node ----------------
static constexpr FeatureInfo FEATS[4] = {
  {"i", "A"}, {"slip", "Hz"}, {"vdc", "V"}, {"tq", "%"},
};
static const FeatureProfile PROF = {"fake_vfd", 4, FEATS};

static void sample(float* x, long t, bool anomaly) {
  x[0] = 3.0f + wave((float)t, 37, 0.10f) + noise(0.05f);   // i [A]
  x[1] = 0.25f + wave((float)t, 53, 0.06f) + noise(0.03f);  // slip [Hz]
  x[2] = 585.0f + wave((float)t, 71, 2.5f) + noise(1.0f);   // vdc [V]
  x[3] = 45.0f + wave((float)t, 29, 3.0f) + noise(1.5f);    // tq [%]
  if (anomaly) { x[0] += 1.2f; x[1] += 0.9f; x[2] -= 15.0f; x[3] += 30.0f; }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  TinyRLConditionMonitor mon;
  mon.begin(PROF);
  mon.startLearning();

  float x[4];
  TinyRLConditionMonitor::StepOut out;

  // ---- A: CAL (300) + FIT (120) ----------------------------------------
  long t = 0;
  for (; t < 300; ++t) { sample(x, t, false); mon.step(x, out); }
  assert(mon.space().armed());
  printf("A: CAL done, ranges frozen\n");
  for (; t < 300 + 120; ++t) { sample(x, t, false); mon.step(x, out); }
  assert(mon.thetaReady());
  assert(!mon.alarmed());
  printf("A: theta=%.3f score=%.3f conf=%.2f state=%s\n",
         mon.theta(), mon.score(), mon.confidence(), mon.stateString());

  // ---- B: 200 quiet steps -> no alarm -----------------------------------
  bool false_alarm = false;
  for (; t < 300 + 120 + 200; ++t) {
    sample(x, t, false);
    mon.step(x, out);
    if (out.alarm_open) false_alarm = true;
  }
  assert(!false_alarm && !mon.alarmed());
  printf("B: 200 quiet steps OK, score=%.3f\n", mon.score());

  // ---- C: anomaly -> alarm ----------------------------------------------
  int alarm_at = -1;
  float x_anom[4];
  for (int k = 0; k < 100; ++k, ++t) {
    sample(x, t, true);
    mon.step(x, out);
    if (out.alarm_open) { alarm_at = k; memcpy(x_anom, x, sizeof(x)); break; }
  }
  assert(mon.alarmed() && alarm_at >= 0);
  printf("C: ALARM opened after %d anomaly steps, score=%.3f theta=%.3f\n",
         alarm_at, mon.score(), mon.theta());

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

  // ---- D: persistence round-trip ----------------------------------------
  assert(persist::modelSave(mon));
  assert(persist::netSave(mon));
  float theta_ref = mon.theta();

  TinyRLConditionMonitor mon2;
  mon2.begin(PROF);
  assert(persist::modelLoad(mon2));
  assert(persist::netLoad(mon2));
  assert(mon2.thetaReady());
  assert(fabsf(mon2.theta() - theta_ref) < 1e-6f);
  // same input -> same normalized surprise with restored net
  sample(x, 9999, false);
  TinyRLConditionMonitor::StepOut out2;
  mon.step(x, out2);
  mon2.step(x, out2);
  printf("D: restored theta=%.3f, score match: %.4f vs %.4f\n",
         mon2.theta(), mon.score(), mon2.score());
  assert(fabsf(mon2.score() - mon.score()) < 0.2f);

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
  int m_norm = lib2.match(x, mon2.space());        // normal sample: no match
  printf("E: labeled 'belt_slip' -> persisted -> anomaly matched idx=%d, "
         "normal matched idx=%d\n", m, m_norm);
  assert(m_norm == -1);

  printf("\nALL HOST TESTS PASSED\n");
  return 0;
}
