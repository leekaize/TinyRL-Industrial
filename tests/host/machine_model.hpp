// =====================================================================
// machine_model.hpp -- ONE simulated motor + the two real node sensor
// models, shared by the web bench (bench_host.cpp) and the synthetic
// tour test (tour_test.cpp). Single source of truth for the generator:
// both harnesses must exercise the same fault deviations and noise
// floors, or their verdicts diverge (2026-11-19).
//
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
// =====================================================================
#pragma once
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "sensor_interface.h"

// ---- deterministic noise (per-node LCG state) -------------------------
static float mm_noise(unsigned long& lcg, float scale) {
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

static constexpr float SP_HZ[2] = {5.0f, 20.0f};

enum Fk { FK_NONE, FK_IMB, FK_PHASE };

// ONE machine truth per test binary (static here: each harness TU owns
// exactly one motor)
static bool   machine_running = false;   // boots NOT RUNNING
static int    machine_sp = 0;            // SP_HZ index
static Fk     machine_fk = FK_NONE;

static const char* truth_ctx() {
  return machine_running ? (machine_sp == 0 ? "op_speed_1" : "op_speed_2")
                         : "op_stopped";
}

static const char* truth_name() {
  // USER DECISION 2026-11-19: fault overlays are measurable only while the
  // machine RUNS (the generator no longer changes any stopped measurement),
  // so a stopped machine is truth op_stopped whatever is installed. The
  // earlier variant reported the overlay while stopped and turned every
  // quiet stopped pass into an expected "identification" that never came.
  if (!machine_running) return "op_stopped";
  switch (machine_fk) {
    case FK_IMB:   return "imbalance";
    case FK_PHASE: return "phase_loss";
    default: break;
  }
  return machine_sp == 0 ? "op_speed_1" : "op_speed_2";
}

class ScriptedSensor : public ISensorNode {
public:
  void setProfile(const FeatureProfile* p) { prof_ = p; }
  void setSeed(unsigned long s) { lcg_ = s; }
  void setQuietLog(bool q) { quiet_log_ = q; }   // tests: no [SIM] flood

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
    if (quiet_log_) return 0;
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
    // USER DECISION 2026-11-19: fault overlays are measurable ONLY while
    // the machine RUNS - a stopped machine reads exactly like a stopped
    // machine no matter what is installed (the old vdc -3.0 leak made
    // stopped+phase_loss a different, spurious state).
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
    if (f_out_ >= 0.5f) {                         // fault overlays: running only
      switch (machine_fk) {
        case FK_IMB: i += 0.05f; tq += 1.2f; break;          // barely notices
        case FK_PHASE: i = i * 1.30f; tq -= 12.0f;
                       vdc -= 3.0f; ni = 0.12f; nvdc = 2.5f; break;
        default: break;
      }
    } else {                                      // output disabled
      i = 0.0f; tq = 0.0f;
      ni = 0.02f; ntq = 0.1f;
    }
    f_[0] = i + mm_noise(lcg_, ni);
    f_[1] = fset + mm_noise(lcg_, 0.01f);
    f_[2] = vdc + mm_noise(lcg_, nvdc);
    f_[3] = fabsf(tq + mm_noise(lcg_, ntq));
  }

  void scenario_vib() {
    // rms grows with the 1x mechanical speed; tones land in the FFT band
    // that contains their frequency, so band content MOVES with speed.
    // USER DECISION 2026-11-19: fault overlays are measurable ONLY while
    // the machine RUNS - stopped reads as stopped whatever is installed.
    bool running = f_out_ > 0.5f;
    float f_mech = 0.485f * f_out_;   // 1x (4-pole on a VFD)
    float rms = running ? 0.005f + 0.0048f * f_mech : 0.004f;
    switch (machine_fk) {
      case FK_IMB: if (running) rms *= 1.5f; break;
      case FK_PHASE: if (running) rms *= 1.3f; break;
      default: break;
    }
    float r2 = rms * rms;
    float b[4] = {0, 0, 0, 0};
    add_tone(f_mech, 0.55f * r2, b);           // 1x running speed
    add_tone(2 * f_mech, 0.12f * r2, b);       // 2x
    add_tone(3 * f_mech, 0.05f * r2, b);       // 3x
    for (int k = 0; k < 4; k++) b[k] += 0.07f * r2;   // broadband floor
    if (running) {
      switch (machine_fk) {
        case FK_IMB: add_tone(f_mech, 0.90f * r2, b); break;    // 1x dominates
        case FK_PHASE:                                          // 2x electrical
          add_tone(2.0f * f_out_, 0.85f * r2, b);
          break;
        default: break;
      }
    }
    // Band estimation noise: a strong tone's band energy repeats within
    // single-digit % between windows (the old +-40% per step put every
    // band ~+-1.2 decades apart at speed - the vib never had 8 consecutive
    // calm steps while running, so it never fed a batch, never matured an
    // ask, and stayed unidentified through every visit, bench 2026-11-19).
    for (int k = 0; k < 4; k++) b[k] *= (1.0f + mm_noise(lcg_, 0.08f));
    f_[0] = rms * (1.0f + mm_noise(lcg_, 0.1f));
    f_[1] = b[0]; f_[2] = b[1]; f_[3] = b[2]; f_[4] = b[3];
    f_[5] = f_out_ < 0.5f ? 0.8f + mm_noise(lcg_, 0.3f)          // floor:
          // stopped jitter is a few %, not +-190% (old 1.5 noise on a 0.8
          // signal put one feature's noise above every match radius and
          // dominated the vib's whole condition space, bench 2026-11-19)
          : machine_fk == FK_PHASE
            ? 2.0f * f_out_ + mm_noise(lcg_, 0.5f)                // 2x elec
            : f_mech + mm_noise(lcg_, 0.3f);                      // 1x
  }

  const FeatureProfile* prof_ = &PROF_G100;
  unsigned long lcg_ = 12345;
  bool quiet_log_ = false;
  float f_[8] = {0};
  long step_ = 0;
  char label_buf_[48] = "";   // compound CI truth label
  float f_ref_ = 0.0f;    // commanded speed (Hz), 0 = not running
  float f_out_ = 0.0f;    // actual speed (Hz), ramps toward f_ref_
  float ramp_rate_ = 0.0f;
};
