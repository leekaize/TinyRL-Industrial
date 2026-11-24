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
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <random>
#include <algorithm>
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

// REAL-data profiles: what the nodes actually deploy since the
// 2026-11-21 feature lock (feature_selection.py study on the real rig
// capture). The g100 fixture predates its own reprofile - the rig tour
// recorded {fset, v, i, pf}, not the sim's {i, slip, vdc, tq}.
// The vib profile was RE-LOCKED 2026-11-22 on the clean re-capture
// (sample-beat fix landed; the 2026-11-21 vib lock ranked features from
// the corrupted capture) - see vibration_sensor.h for the provenance.
static constexpr FeatureInfo FEATS_G100_REAL[4] = {
  {"fset", "Hz"}, {"v", "V"}, {"i", "A"}, {"pf", ""},
};
static constexpr FeatureProfile PROF_G100_REAL = {"vfd_g100", 4,
                                                  FEATS_G100_REAL};
static constexpr FeatureInfo FEATS_VIB_REAL[4] = {
  {"rms", "g"}, {"b2", "g2"}, {"rms_y", "g"}, {"centroid", "Hz"},
};
static constexpr FeatureProfile PROF_VIB_REAL = {"vib_adxl", 4,
                                                 FEATS_VIB_REAL};

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

// ---- real-data replay (DEFAULT since 2026-11-22c) -------------------
// The SAME ScriptedSensor streams REAL recordings per condition instead
// of the synthetic generator (2026-11-21). User decision 2026-11-22c:
// real-world data is the DEFAULT for every test and figure; recordings
// come from the MODBUS collect campaign. Per modality:
//   g100: TINYRL_G100_CSV (default data/serial/collect_20261121_211534.csv,
//         the collect campaign; 5 LABELED conditions, ~150 rows each.
//         Labeled-only per user decision 2026-11-22d: the campaign's
//         unlabeled rows are the machine mid-move under an unpressed
//         ENTER - unconfirmed state, so they no longer become transition
//         ramps. Machine switches jump straight to the destination block;
//         a future script generates the ramps (always 1 second between
//         two states) into RealTrans directly)
//   vib:  TINYRL_VIB_CSV (default data/analysis/vib_4feat.csv, the
//         feature-selection capture export; not captured yet)
// A modality whose recording is missing (or pinned synthetic, below)
// falls back to its SYNTHETIC profile AND generator together (a real
// profile over the synthetic generator's 6-feature values would misread
// b1/b2/b3 as entropy/margin/rms_x - degenerate, found in the first
// mixed run).
//   TINYRL_SYNTH=1     forces the synthetic world everywhere (the old
//                      default; a debugging escape hatch)
//   TINYRL_VIB_SYNTH=1 pins the VIBRATION node to its synthetic generator
//                      until its re-capture lands and is trusted; every
//                      other modality stays real. Unset it (or set 0)
//                      once data/analysis/vib_4feat.csv is real.
// A condition with no recording for this node HOLDS the node's previous
// block - the node's world simply did not change (the drive cannot see
// imbalance; speed_2 was never recorded on the drive side). One row per
// poll = one captured window per model step (rows are 2 s snapshots);
// a block shorter than its stage CYCLES: the recordings are steady
// states, so repetition adds no synthetic data (user decision). Columns
// are matched by HEADER NAME to the profile, so any export works.
//
// TRANSITIONS (2026-11-22, user request): the unlabeled rows between two
// labeled blocks are the machine's real ramps (setpoint moves, starts,
// fault installs). The loader buckets them per (from,to) condition pair
// in file order; a machine switch that has a recorded ramp streams it
// first (once per switch - a transition is an event, not a state), then
// the destination block resumes. Rows before the first label (pre-
// campaign warm-up) are dropped.
struct RealBlock { std::vector<std::vector<float>> rows; };
struct RealTrans { std::vector<std::vector<float>> rows; };

// 0 op_stopped, 1 speed_1, 2 speed_2, 3 speed_1_imbalance,
// 4 speed_1_phase_loss, 5 speed_2_imbalance, 6 speed_2_phase_loss
static const char* MM_COND_NAMES[7] = {
  "op_stopped", "speed_1", "speed_2", "speed_1_imbalance",
  "speed_1_phase_loss", "speed_2_imbalance", "speed_2_phase_loss",
};

static int mm_cond_index() {
  if (!machine_running) return 0;
  if (machine_fk == FK_IMB) return machine_sp == 0 ? 3 : 5;
  if (machine_fk == FK_PHASE) return machine_sp == 0 ? 4 : 6;
  return machine_sp == 0 ? 1 : 2;
}

static RealBlock mm_g100_blocks[7];
static RealBlock mm_vib_blocks[7];
static RealTrans mm_g100_trans[7][7];
static RealTrans mm_vib_trans[7][7];
static bool mm_g100_ok = false, mm_vib_ok = false;

// Split a CSV line PRESERVING empty fields (strtok merges ",," and
// would shift every transition row left by one column - found 2026-11-22
// when the collect campaign's 152 unlabeled ramp rows all landed in the
// unknown-label skip). Returns the field count; \r/\n are zeroed.
static int mm_split(char* s, char** fld, int cap) {
  for (char* p = s; *p; ++p)
    if (*p == '\r' || *p == '\n') *p = 0;
  int n = 1;
  fld[0] = s;
  for (char* p = s; *p; ++p)
    if (*p == ',' && n < cap) { *p = 0; fld[n++] = p + 1; }
  return n;
}

// trans is left UNFILLED by the loader (labeled-only, see the loop
// below); mm_gen_ramps synthesizes the 1-second ramps after the load.
static bool mm_load_csv(const char* path, const FeatureProfile* prof,
                        RealBlock* out, RealTrans (*trans)[7]) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[1024];
  if (!fgets(line, sizeof(line), f)) { fclose(f); return false; }
  // header: find the label column (truth|label) + one column per
  // profile feature, by NAME. NB: the empty label of a transition row is
  // a real field - mm_split preserves it (the strtok version silently
  // skipped every transition row).
  int labcol = -1, colidx[8] = {0};
  {
    char buf[1024], *names[64];
    int nn = 0;
    snprintf(buf, sizeof(buf), "%s", line);
    nn = mm_split(buf, names, 64);
    for (int i = 0; i < nn; ++i) {
      if (strcmp(names[i], "truth") == 0 || strcmp(names[i], "label") == 0)
        labcol = i;
      for (int fj = 0; fj < prof->n_features; ++fj)
        if (strcmp(names[i], prof->features[fj].name) == 0) colidx[fj] = i;
    }
    if (labcol < 0) { fclose(f); return false; }
  }
  int skipped = 0, nunlabeled = 0;
  auto parse_row = [&](char** fld) {
    std::vector<float> r((size_t)prof->n_features);
    for (int fj = 0; fj < prof->n_features; ++fj)
      r[fj] = strtof(fld[colidx[fj]], nullptr);
    return r;
  };
  while (fgets(line, sizeof(line), f)) {
    char* fld[64];
    int nf = mm_split(line, fld, 64);
    if (nf <= labcol) { skipped++; continue; }
    // LABELED-ONLY (user decision 2026-11-22d): the campaign's unlabeled
    // rows are the machine mid-move under an unpressed ENTER - the
    // operator had already commanded the transition, so the row's true
    // state is unconfirmed. They no longer become transition ramps:
    // machine switches jump straight to the destination block (the
    // settle gate covers the jump), and a future script-generated ramp
    // (always 1 second between two states) fills RealTrans directly.
    if (!fld[labcol][0]) { nunlabeled++; continue; }
    int idx = -1;
    for (int c = 0; c < 7; ++c)
      if (!strcmp(fld[labcol], MM_COND_NAMES[c])) idx = c;
    if (idx < 0) { skipped++; continue; }
    out[idx].rows.push_back(parse_row(fld));
  }
  fclose(f);
  fprintf(stderr, "REALMODE %s: ", path);
  for (int c = 0; c < 7; ++c)
    fprintf(stderr, "%s=%zu ", MM_COND_NAMES[c], out[c].rows.size());
  fprintf(stderr, "labeled-only: unlabeled_rows=%d skipped=%d\n",
          nunlabeled, skipped);
  return true;
}

static void mm_gen_ramps(RealBlock* blocks, RealTrans (*trans)[7],
                         bool resolve);

static void mm_real_load() {
  const char* g = getenv("TINYRL_G100_CSV");
  const char* v = getenv("TINYRL_VIB_CSV");
  // the canonical model-testing dataset (user point-out, 2026-11-21):
  // the collect campaign, 150 settled rows per condition
  mm_g100_ok = mm_load_csv(
      g && *g ? g : "data/serial/collect_20261121_211534.csv",
      &PROF_G100_REAL, mm_g100_blocks, mm_g100_trans);
  const char* vs = getenv("TINYRL_VIB_SYNTH");
  bool vib_pin = vs && *vs && strcmp(vs, "0") != 0;
  if (vib_pin) {
    mm_vib_ok = false;                 // pinned synthetic until the capture
                                       // lands and is trusted (user decision
                                       // 2026-11-22c: the parameter to flip
                                       // back is unsetting this var)
    fprintf(stderr, "[MM] vib: SYNTHETIC (pinned by TINYRL_VIB_SYNTH=1 -"
                    " unset it once %s is captured and trusted)\n",
            v && *v ? v : "data/analysis/vib_4feat.csv");
  } else {
    mm_vib_ok = mm_load_csv(v && *v ? v : "data/analysis/vib_4feat.csv",
                            &PROF_VIB_REAL, mm_vib_blocks, mm_vib_trans);
    if (!mm_vib_ok)
      fprintf(stderr, "[MM] vib: SYNTHETIC (no capture at %s - re-record"
                      " with tools/feature_selection.py --capture)\n",
              v && *v ? v : "data/analysis/vib_4feat.csv");
  }
  if (!mm_g100_ok)
    fprintf(stderr, "[MM] g100: SYNTHETIC (no recording at %s - re-run"
                    " tools/collect_data.py)\n",
            g && *g ? g : "data/serial/collect_20261121_211534.csv");
  // 1-second transition ramps (user decision 2026-11-22d, replaces the
  // operator-paced unlabeled rows): every ordered pair of recorded
  // blocks gets ONE synthesized midpoint row - the single second between
  // two states, linear in raw feature space between the confirmed tail
  // of the source block and the confirmed head of the destination. No
  // unconfirmed row ever reaches the model, and machine switches stay
  // fast (the settle gate covers the 1-row transient).
  // DEFAULT ON since 2026-11-22g: the physical machine ramps ~1 s between
  // states, so the fixture is honest only with the ramp row present.
  // The churn race it exposed is fixed at the fire check: the score arm
  // yields while the geometry is quiet (floor-move case), so the
  // re-anchor owns sustained score excess over quiet geometry and the
  // ramp row's band charge never alarms (TINYRL_RAMP_SYNTH=0 ablates
  // back to the instantaneous-switch fixture).
  // CHUNK SHUFFLE (user request 2026-11-22): the blocks replay in capture
  // order, so every block loop redraws the same waveform cycle on the
  // figures. Shuffle CHUNK-row chunks with a FIXED seed - the paper runs
  // must reproduce - so the replay samples the same distribution without
  // the visible cycle. The rows of a steady-state block are exchangeable
  // samples of one condition; chunks keep local continuity (a row and its
  // 2-second neighbours stay adjacent). Runs before the ramp synthesis:
  // the ramp midpoints then use representative (shuffled) tail/head rows.
  {
    constexpr size_t MM_SHUFFLE_CHUNK = 5;
    std::mt19937 rng(20261122u);
    RealBlock* block_sets[2] = { mm_g100_blocks, mm_vib_blocks };
    for (RealBlock* blocks : block_sets) {
      for (int c = 0; c < 7; ++c) {
        auto& rows = blocks[c].rows;
        if (rows.size() < 2 * MM_SHUFFLE_CHUNK) continue;
        std::vector<std::pair<size_t, size_t>> chunks;   // [begin,end) per chunk
        for (size_t i = 0; i < rows.size(); i += MM_SHUFFLE_CHUNK)
          chunks.emplace_back(i, std::min(i + MM_SHUFFLE_CHUNK, rows.size()));
        std::shuffle(chunks.begin(), chunks.end(), rng);
        std::vector<std::vector<float>> shuffled;
        shuffled.reserve(rows.size());
        for (auto& ck : chunks)
          for (size_t i = ck.first; i < ck.second; ++i)
            shuffled.push_back(std::move(rows[i]));
        rows = std::move(shuffled);
      }
    }
  }
  if (getenv("TINYRL_RAMP_SYNTH") == nullptr ||
      strcmp(getenv("TINYRL_RAMP_SYNTH"), "0") != 0) {
    if (mm_g100_ok) mm_gen_ramps(mm_g100_blocks, mm_g100_trans, true);
    if (mm_vib_ok) mm_gen_ramps(mm_vib_blocks, mm_vib_trans, false);
    fprintf(stderr, "[MM] ramps: 1-second midpoint rows synthesized per"
                    " ordered block pair (default; TINYRL_RAMP_SYNTH=0"
                    " disables)\n");
  }
}

// USER DECISION 2026-11-21: the drive's features are identical under
// imbalance, so the g100 replays the matching speed block for those
// stages (speed_1_imbalance -> speed_1 rows, speed_2_imbalance ->
// speed_2). The vib has real imbalance recordings and resolves 1:1.
static int mm_resolve(bool g100, int c) {
  if (!g100) return c;
  return c == 3 ? 1 : c == 5 ? 2 : c;
}

// 1-second transition ramps (user decision 2026-11-22d): ONE synthesized
// midpoint row per ordered pair of recorded blocks, linear in raw feature
// space between the confirmed tail of the source and the confirmed head
// of the destination. resolve=true folds the imbalance indices onto their
// speed blocks (mm_resolve) so a switch into an unrecorded condition
// still replays the ramp of the rows it actually serves.
static void mm_gen_ramps(RealBlock* blocks, RealTrans (*trans)[7],
                         bool resolve) {
  for (int a = 0; a < 7; ++a) {
    int ra = resolve ? mm_resolve(true, a) : a;
    if (blocks[ra].rows.empty()) continue;
    for (int b = 0; b < 7; ++b) {
      int rb = resolve ? mm_resolve(true, b) : b;
      if (rb == ra || blocks[rb].rows.empty()) continue;
      const std::vector<float>& tail = blocks[ra].rows.back();
      const std::vector<float>& head = blocks[rb].rows.front();
      std::vector<float> mid(tail.size());
      for (size_t f = 0; f < tail.size(); ++f)
        mid[f] = 0.5f * (tail[f] + head[f]);
      trans[ra][rb].rows.clear();
      trans[ra][rb].rows.push_back(mid);
    }
  }
}

// Default REAL (user decision 2026-11-22c): every test and figure runs
// recorded rig data. TINYRL_SYNTH=1 restores the synthetic world for
// debugging; TINYRL_VIB_SYNTH=1 pins only the vibration modality.
static bool mm_real_on() { return getenv("TINYRL_SYNTH") == nullptr; }
// Profile selection happens BEFORE begin() loads the CSVs, so the load
// is lazy here: a modality whose recordings are missing falls back to
// the SYNTHETIC profile AND generator together (a real profile over the
// synthetic generator's 6-feature values would misread b1/b2/b3 as
// entropy/margin/rms_x - degenerate, found in the first mixed run).
static void mm_ensure_loaded() {
  static bool done = false;
  if (!done) { done = true; if (mm_real_on()) mm_real_load(); }
}
static const FeatureProfile* mm_prof_g100() {
  mm_ensure_loaded();
  return (mm_real_on() && mm_g100_ok) ? &PROF_G100_REAL : &PROF_G100;
}
static const FeatureProfile* mm_prof_vib() {
  mm_ensure_loaded();
  return (mm_real_on() && mm_vib_ok) ? &PROF_VIB_REAL : &PROF_VIB;
}
static bool mm_has_block(bool g100, const char* slug) {
  for (int c = 0; c < 7; ++c)
    if (strcmp(MM_COND_NAMES[c], slug) == 0) {
      int r = mm_resolve(g100, c);
      return g100 ? !mm_g100_blocks[r].rows.empty()
                  : !mm_vib_blocks[r].rows.empty();
    }
  return false;
}

class ScriptedSensor : public ISensorNode {
public:
  void setProfile(const FeatureProfile* p) { prof_ = p; }
  void setSeed(unsigned long s) { lcg_ = s; }
  void setQuietLog(bool q) { quiet_log_ = q; }   // tests: no [SIM] flood

  bool begin() override {
    step_ = 0;
    mm_ensure_loaded();
    real_ = (prof_ == &PROF_G100_REAL && mm_g100_ok) ||
            (prof_ == &PROF_VIB_REAL && mm_vib_ok);
    last_cond_ = mm_cond_index();
    return true;
  }
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
    int o = snprintf(out, cap, "%s t=%ld", real_ ? "[REAL]" : "[SIM]", step_);
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

  // command the machine; the speed itself ramps, the fault overlays.
  // Real mode: the machine state selects the recorded block to stream.
  void setMachine(bool run, int sp, Fk fk) {
    if (real_) {
      bool g100 = prof_ == &PROF_G100_REAL;
      int c = mm_cond_index();       // globals already updated by the caller
      if (c != last_cond_) {
        int fr = g100 ? mm_resolve(true, last_cond_) : last_cond_;
        int to = g100 ? mm_resolve(true, c) : c;
        const auto& tr = g100 ? mm_g100_trans[fr][to].rows
                              : mm_vib_trans[fr][to].rows;
        trans_ = tr.empty() ? nullptr : &tr;   // ramp replays once per switch
        trans_row_ = 0;
        last_cond_ = c;
        cur_row_ = 0;
      }
      return;
    }
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
    if (real_) { scenario_real(); return; }
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

  // real mode: serve the current row of the condition's recording; a
  // recorded machine ramp (transition) replays first on a switch; a
  // missing block HOLDS the previous features (world unchanged); a short
  // block cycles (steady states - repetition adds no synthetic data)
  void scenario_real() {
    bool g100 = prof_ == &PROF_G100_REAL;
    if (trans_) {
      if (trans_row_ < trans_->size()) {
        for (int i = 0; i < prof_->n_features; ++i)
          f_[i] = (*trans_)[trans_row_][i];
        ++trans_row_;
        return;
      }
      trans_ = nullptr;              // ramp done: destination block resumes
    }
    int r = g100 ? mm_resolve(true, mm_cond_index()) : mm_cond_index();
    const RealBlock& blk = g100 ? mm_g100_blocks[r] : mm_vib_blocks[r];
    if (blk.rows.empty()) return;
    for (int i = 0; i < prof_->n_features; ++i)
      f_[i] = blk.rows[stream_row_(r, cur_row_, blk.rows.size(), g100)][i];
    ++cur_row_;                    // raw cursor: stream_row_ derives the
                                   // loop and the in-loop position
  }

  // Per-loop chunk permutation (user request 2026-11-22, refined the same
  // evening: the fixed one-shot shuffle still redrew the same 15-row
  // cycle on every block loop - clearly visible on the figures). Every
  // loop of a block re-draws a chunk-5 order seeded by (paper seed,
  // modality, condition, loop): deterministic, so the paper runs
  // reproduce, but no two consecutive loops share an order, so no
  // period-n repetition survives on a figure. The load-time shuffle
  // stays: it fixes the ramp endpoints (rows.back()/front()).
  size_t stream_row_(int r, size_t raw, size_t n, bool g100) {
    if (n < 10) return raw % n;    // too short to chunk meaningfully
    size_t loop = raw / n;
    int mod = g100 ? 0 : 1;
    if (order_cond_ != r || order_mod_ != mod || order_loop_ != loop ||
        order_.size() != n) {
      constexpr size_t CHUNK = 5;
      bool need_diff = (order_cond_ == r && order_mod_ == mod &&
                        order_.size() == n && order_loop_ + 1 == loop);
      unsigned seed = 20261122u
                    ^ ((unsigned)(r + 7 * mod)) * 2654435761u
                    ^ (unsigned)loop * 40503u;
      do {
        std::vector<size_t> chunks;          // chunk start offsets
        for (size_t i = 0; i < n; i += CHUNK) chunks.push_back(i);
        std::mt19937 rng(seed++);
        std::shuffle(chunks.begin(), chunks.end(), rng);
        order_.clear();
        order_.reserve(n);
        for (size_t cs : chunks)
          for (size_t off = 0; off < CHUNK && cs + off < n; ++off)
            order_.push_back(cs + off);
      } while (need_diff && order_ == prev_order_);
      prev_order_ = order_;
      order_cond_ = r;
      order_mod_ = mod;
      order_loop_ = loop;
    }
    return order_[raw % n];
  }

  const FeatureProfile* prof_ = &PROF_G100;
  unsigned long lcg_ = 12345;
  bool quiet_log_ = false;
  bool real_ = false;             // TINYRL_REAL=1 && a real profile
  size_t cur_row_ = 0;            // raw cursor into the current real block
  std::vector<size_t> order_;     // stream order of the current block loop
  std::vector<size_t> prev_order_;
  int order_cond_ = -1;
  int order_mod_ = -1;
  size_t order_loop_ = 0;
  const std::vector<std::vector<float>>* trans_ = nullptr;  // active ramp
  size_t trans_row_ = 0;          // cursor into the active ramp
  int last_cond_ = 0;             // condition the cursor belongs to
  float f_[8] = {0};
  long step_ = 0;
  char label_buf_[48] = "";   // compound CI truth label
  float f_ref_ = 0.0f;    // commanded speed (Hz), 0 = not running
  float f_out_ = 0.0f;    // actual speed (Hz), ramps toward f_ref_
  float ramp_rate_ = 0.0f;
};
