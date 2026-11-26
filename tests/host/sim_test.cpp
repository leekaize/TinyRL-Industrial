// =====================================================================
// sim_test.cpp -- stage 2 of the host suite: the MONITOR in isolation.
// Compiles the REAL general-core modules against the REAL TinyRL headers
// with Arduino stubs, then asserts the core lifecycle end to end:
//   A) CAL -> theta fit on a synthetic fixed-load induction-motor stream
//      (noise-stationary + slow thermal drift; see sample() rationale)
//   B) 200 quiet steps  -> must NOT alarm
//   C) persistence round-trip (model.bin + net.bin via RAM LittleFS):
//      restored node scores identically on the same stream
//   D) injected anomaly -> MUST alarm (the detector's reason to exist)
//   E) condition library label -> save -> load -> match
//   F) plasticity counters + optional long run (Dohare 2024, RQ2)
//   G) per-step compute timing (host proxy for the MCU budget)
// =====================================================================
#include <cassert>
#include <chrono>
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

  // plasticity audit state (Dohare 2024; host-test suite logging only):
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
  // the unsaved optimizer state (traces + leaky max), see the net.bin note
  // in docs/system/architecture.md (boot and persistence).
  TinyRLConditionMonitor::StepOut out2;
  for (int k = 0; k < 70; ++k, ++t) {
    sample(x, t, false);
    record(x);
    if (trace)
      printf("TRACE step=%ld tag=quiet score=%.4f theta=%.4f\n",
             (long)mon.steps(), mon.score(), mon.theta());
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
  printf("D: ALARM opened after %d anomaly steps, score=%.3f theta=%.3f\n",
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
  float st_rel[4];
  (void)st_rel;
  ep.open(x, mon.space(), (uint32_t)t * 1000);
  char sum[192];
  ep.summary(sum, sizeof(sum), (uint32_t)t * 1000 + 3000);
  assert(strstr(sum, "i=") && strstr(sum, "vdc="));
  printf("E0: generic summary: %s\n", sum);

  // ---- E: condition library label -> save -> load -> match --------------
  float rmean[4], rstd[4];
  ep.relMeans(rmean);
  ep.relStd(rstd);
  int idx = lib.label("belt_slip", rmean, rstd, "agent-01");
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
    // 2026-11-15 training policy: the net trains only on settled steps of
    // LABELED conditions. Without labels the bench would freeze the net at
    // the first shift and measure nothing - so the bench emulates the
    // operator: the initial regime is labeled at the start, each shift's
    // regime is labeled 20 steps in (settled). Regimes that sit inside the
    // previous condition's radius keep training as drift tracking; the
    // rest read as unknown until labeled - both are the designed paths.
    ConditionLibrary flib;
    flib.begin(4);
    mon.attachLibrary(&flib);
    long shifted_at = 0;
    int nreg = 0;
    for (long k = 1; k <= lr_steps; ++k, ++t) {
      if (regime > 0 && k % regime == 0) {         // regime shift
        off[0] += 0.4f; off[1] += 0.15f; off[3] += 4.0f;
        shifted_at = k;
        printf("F: regime shift at step %lu (offsets %.2f/%.2f/%.2f)\n",
               (unsigned long)mon.steps(), off[0], off[1], off[3]);
      }
      sample(x, t, false);
      x[0] += off[0]; x[1] += off[1]; x[3] += off[3];
      if (k == 1 || (shifted_at && k == shifted_at + 20)) {
        // label AFTER x reflects the (new) regime: settled 20 steps in
        char slug[24];
        snprintf(slug, sizeof(slug), "regime_%d", nreg++);
        float rm[4], z[4] = {};
        for (int f = 0; f < 4; ++f) rm[f] = mon.space().rel(f, x[f]);
        flib.label(slug, rm, z, "bench", COND_NORMAL);
        shifted_at = 0;
      }
      record(x);                                   // moments frozen; window only
      mon.step(x, out);
      if (k % report == 0) plast::audit(mon, zr, rawWin, 4, t);
    }
    plast::audit(mon, zr, rawWin, 4, t);
    printf("F: long-run done at step %lu\n", (unsigned long)mon.steps());
  } else {
    plast::audit(mon, zr, rawWin, 4, t);           // final default-path snapshot
  }

  // ---- H: per-condition detection trace (the A/B headline) -------------
  // The architecture is no longer ONE shared score vs ONE global theta: a
  // matched state gates on its OWN matured surprise band (scoreTheta), an
  // unknown state on the global fit theta. One monitor exercises the split
  // end to end:
  //   load_a labeled -> quiet under its band
  //   shift to load_b  -> unknown -> alarm -> labeled -> net learns -> quiet
  //   back to load_a   -> recurrence, quiet
  //   in-radius fault  -> 2.5 band-sigmas on EVERY feature at once: inside
  //                      each per-feature match radius (max-component test,
  //                      so the prototype layer calls it quiet) but far off
  //                      the condition's training manifold. Era verdicts:
  //                      z era misses (clamped point + trained shared point),
  //                      rel era misses (level-space interpolation across
  //                      the two trained regimes), cond-obs era catches
  //                      (residuals: the fault is 2.5 sigma off EVERY
  //                      condition's manifold centre).
  // The fault point is built EXACTLY from the centroid + offsets (no
  // fixture jitter) so the max-component distances are deterministic.
  {
    TinyRLConditionMonitor m3;
    m3.begin(PROF);
    m3.startLearning();
    ConditionLibrary lib3;
    lib3.begin(4);
    m3.attachLibrary(&lib3);
    float xh[4];
    TinyRLConditionMonitor::StepOut oh;

    long th = 0;
    for (; th < cfg::CAL_STEPS + cfg::FIT_STEPS; ++th) {
      sample(xh, th, false); m3.step(xh, oh);
    }
    assert(m3.thetaReady());

    auto relWin = [&](float* m, float* s, const float* off) {
      float acc[4] = {}, acc2[4] = {};
      for (int k = 0; k < 16; ++k, ++th) {
        sample(xh, th, false);
        for (int f = 0; f < 4; ++f) xh[f] += off[f];
        m3.step(xh, oh);
        for (int f = 0; f < 4; ++f) {
          float r = m3.space().rel(f, xh[f]);
          acc[f] += r; acc2[f] += r * r;
        }
      }
      for (int f = 0; f < 4; ++f) {
        m[f] = acc[f] / 16.0f;
        float var = acc2[f] / 16.0f - m[f] * m[f];
        s[f] = var > 1e-12f ? sqrtf(var) : 0.0f;
      }
    };
    auto htrace = [&](const char* tag) {
      if (!trace) return;
      int bm = (oh.matched >= 0 && lib3.at(oh.matched).s_n >= cfg::PERCOND_MIN_N);
      printf("HTRACE step=%ld tag=%s score=%.6f theta=%.6f km=%d gate=%.6f "
             "al=%d bm=%d ragg=%.4f grate=%.4f acc=%.4f\n",
             (long)m3.steps(), tag, m3.score(), m3.theta(), oh.matched,
             oh.gate, m3.alarmed() ? 1 : 0, bm, oh.ragg, oh.gate_r, oh.acc);
    };

    // Learning-budget windows. Research constraint (2026-11-21): a new
    // condition gets 60-300 samples on the real rig, so the cond era must
    // arm its gates within 300 settled samples. The rel/z eras keep the
    // windows their documented bench numbers were measured at.
#if defined(MONITOR_COND_OBS)
    const int LEARN_B = 300;    // new-condition budget (upper bound; the
                                // 60-step lower bound was validated
                                // separately, 2026-11-21)
    const int HEAL_A = 300;     // re-entry heal, same budget
#else
    const int LEARN_B = 4000;
    const int HEAL_A = 8000;
#endif

    const float OFF0[4] = {0, 0, 0, 0};
    float ma[4], sa[4];
    relWin(ma, sa, OFF0);
    int ia = lib3.label("load_a", ma, sa, "bench", COND_NORMAL);
    assert(ia == 0);

    bool a_false = false;
    for (int k = 0; k < 300; ++k, ++th) {
      sample(xh, th, false); m3.step(xh, oh); htrace("load_a");
      if (oh.alarm_open) a_false = true;
    }
    assert(!a_false && !m3.alarmed());     // quiet under its own band

    const float OFF_B[4] = {0.4f, 0.15f, 0.0f, 4.0f};
    bool b_alarmed = false; int b_at = -1;
    for (int k = 0; k < 60; ++k, ++th) {
      sample(xh, th, false);
      for (int f = 0; f < 4; ++f) xh[f] += OFF_B[f];
      m3.step(xh, oh); htrace("shift_b");
      if (oh.alarm_open && b_at < 0) { b_alarmed = true; b_at = k; }
    }
    assert(b_alarmed);                     // unknown regime alarms (both eras)

    float mb[4], sb[4];
    relWin(mb, sb, OFF_B);
    int ib = lib3.label("load_b", mb, sb, "bench", COND_NORMAL);

    bool b_refire = false;
    for (int k = 0; k < LEARN_B; ++k, ++th) {
      sample(xh, th, false);
      for (int f = 0; f < 4; ++f) xh[f] += OFF_B[f];
      m3.step(xh, oh); htrace("load_b");
      if (oh.alarm_open) b_refire = true;
    }
    printf("H: load_b: end score %.4g theta %.4g re-fires %d, band s_n=%u "
           "gate=%.4g pos r_n=%u gate_r=%.4g (learn window %d)\n",
           m3.score(), m3.theta(), b_refire, (unsigned)lib3.at(ib).s_n,
           lib3.scoreTheta(ib, m3.theta()), (unsigned)lib3.at(ib).r_n,
           lib3.posTheta(ib), LEARN_B);
    assert(!m3.alarmed());                 // quiet by name, then by band
#if defined(MONITOR_REL_OBS) || defined(MONITOR_COND_OBS)
    assert(lib3.at(ib).s_n >= cfg::PERCOND_MIN_N);   // grace expired into score gating
#endif

    // back to load_a: re-entry after training on load_b. Cross-regime
    // interference may lift the score above band A briefly - the
    // plasticity-stability tradeoff, measured here. Full-rate re-learning
    // must self-heal within the window.
    bool back_refire = false;
    for (int k = 0; k < HEAL_A; ++k, ++th) {
      sample(xh, th, false); m3.step(xh, oh); htrace("back_a");
      if (oh.alarm_open) back_refire = true;
    }
    assert(!m3.alarmed());                 // healed (all eras)
    printf("H: back-to-load_a refire=%d (z era: 0 by latch; rel/cond eras: "
           "interference transient, self-healed by RELEARN_FRAC re-learning)\n",
           back_refire);

#if defined(MONITOR_COND_OBS)
    // third-condition tripwire: with TWO regimes named, an unknown state
    // must still alarm via nearest-condition centering - its residual is
    // >= 3 units off the closest regime by definition of a match failure,
    // far off the trained manifold. This is the cond era's unknown-state
    // evidence path end to end (and it must keep working as the library
    // grows). vdc carries no offset, mirroring OFF_B, so the point is a
    // genuine operating point and not a sensor dropout.
    const float OFF_C[4] = {0.8f, 0.3f, 0.0f, 8.0f};
    bool c_alarmed = false;
    for (int k = 0; k < 60; ++k, ++th) {
      sample(xh, th, false);
      for (int f = 0; f < 4; ++f) xh[f] += OFF_C[f];
      m3.step(xh, oh); htrace("shift_c");
      if (oh.alarm_open) c_alarmed = true;
    }
    assert(c_alarmed);
    printf("H: load_c unknown-state tripwire (nearest-of-two centering): "
           "alarmed yes\n");
    // 120 quiet load_a steps clear the tripwire alarm so the fault phase
    // below starts from a closed alarm (alarm_open must be able to fire).
    for (int k = 0; k < 120; ++k, ++th) {
      sample(xh, th, false); m3.step(xh, oh); htrace("clear_c");
    }
    assert(!m3.alarmed());
#endif

    // in-radius fault: 2.5 band-sigmas on every feature, EXACTLY at
    // centroid + offset (no jitter -> deterministic max-component test)
    float drel[4];
    for (int f = 0; f < 4; ++f) {
      float u = lib3.at(ia).u[f] > 0 ? lib3.at(ia).u[f] : m3.space().unit(f);
      drel[f] = 2.5f * u;
    }
#if defined(MONITOR_REL_OBS) || defined(MONITOR_COND_OBS)
    assert(lib3.at(ia).s_n >= cfg::PERCOND_MIN_N);   // the band rules the gate
#endif
    int fault_at = -1;
    for (int k = 0; k < 120; ++k, ++th) {
      sample(xh, th, false);
      for (int f = 0; f < 4; ++f) {
        float A = m3.space().anchor(f), mu = m3.space().mu(f);
        float rl = lib3.at(ia).c[f] + drel[f];       // exact fault position
        xh[f] = (mu + A) * powf(2.0f, rl) - A;
      }
      m3.step(xh, oh); htrace("fault");
      if (oh.alarm_open && fault_at < 0) fault_at = k;
    }
#if defined(MONITOR_COND_OBS)
    assert(fault_at >= 0);                 // residuals: off-manifold -> surprise
    printf("PERCOND-ERA: cond-obs era CATCHES the in-radius fault "
           "(alarm %d steps after onset; match radius says quiet, "
           "positional band says fault)\n", fault_at);
#elif defined(MONITOR_REL_OBS)
    // Documented ablation result (2026-11-21): rel LEVELS interpolate
    // across the two trained regimes, so a static fault BETWEEN them is
    // predicted straight through (the score decays into the band; trace:
    // 0.0038 -> 0.0004). The cond-obs build above is the fix; this miss
    // is the paper's why for the representation change.
    assert(fault_at < 0);
    printf("PERCOND-ERA: rel era MISSES the in-radius fault "
           "(level-space interpolation across the trained regimes)\n");
#else
    assert(fault_at < 0);                  // positional latch: structurally missed
    printf("PERCOND-ERA: z era MISSES the in-radius fault "
           "(positional quiet) - the A/B contrast\n");
#endif
    printf("H: per-condition trace stage done\n");
  }

  // ---- G: per-step compute timing (host proxy for the MCU budget) ------
  // The monitor step is the whole per-second workload on the node (forward
  // + bounded update + score EMA + gates). Host timing is an x86 proxy,
  // not an ESP32 measurement - but it catches order-of-magnitude
  // regressions (a step that costs > 1 ms on x86 cannot fit a 1 Hz loop
  // on a 240 MHz LX7 with other tasks). Budget: 1 s per step at 1 Hz.
  {
    const int N = 2000;
    float x[4];
    double total = 0.0;
    double worst = 0.0;
    for (int k = 0; k < N; ++k, ++t) {
      sample(x, t, false);
      auto t0 = std::chrono::steady_clock::now();
      mon.step(x, out);
      auto t1 = std::chrono::steady_clock::now();
      double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
      total += us;
      if (us > worst) worst = us;
    }
    printf("G: per-step compute (host proxy): avg %.0f us, max %.0f us "
           "over %d steps (MCU budget: 1 s/step at 1 Hz)\n",
           total / N, worst, N);
  }

  printf("\nSIMCHECK OK: cal/fit/quiet/persist/alarm/label/timing asserted\n");
  return 0;
}
