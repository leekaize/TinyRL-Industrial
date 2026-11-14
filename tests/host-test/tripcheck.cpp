// =====================================================================
// tripcheck.cpp -- behavioral check of the quiet-unmatched tripwire
// (Settles 2012 ch. 2.5 myopia counter, RESEARCH.md design decisions)
// running on the REAL SensorNodeApp core with a fake ISensorNode
// (no Modbus) and the stub WiFi/MQTT/LittleFS.
//
// Scenario (baselines in raw units; the 0..1 condition space saturates
// at the CAL range edges, which is what separates matched from unmatched):
//   0) console 's': CAL + FIT at NORMAL (full sensor noise) -> theta
//   1) step to L1 = NORMAL + 3 sigma (smooth, fault-like): z-jump alarms
//      -> console 'f belt_slip' -> library = 1, centroid at L1
//   2) stay at L1: net adapts, alarm clears, quiet steps MATCH the
//      centroid (distance ~ noise only) -> tripwire silent (qum=0)
//   3) step to L2 = NORMAL - 3 sigma (smooth): alarms, adapts, clears;
//      quiet steps sit ~0.87 from the centroid in 0..1 space -> no match
//      within r=0.30 -> report line fires, qum climbs
// Local log only; nothing published, no asks.
//
// Serial/stdout is redirected to a temp log and scanned between PHASE
// markers, so the assertions see the app's own console output.
// =====================================================================
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "node_runtime.h"

static constexpr FeatureInfo FEATS[4] = {
  {"i", "A"}, {"slip", "Hz"}, {"vdc", "V"}, {"tq", "%"}};
static const FeatureProfile PROF = {"fake_vfd", 4, FEATS};

class FakeSensor : public ISensorNode {
public:
  float base[4] = {3.0f, 0.25f, 585.0f, 45.0f};   // NORMAL load
  float cur[4]  = {3.0f, 0.25f, 585.0f, 45.0f};
  float nscale = 1.0f;                            // noise multiplier (smooth regimes < 1)
  mutable long feeds = 0;                          // features() = monitor steps

  bool begin() override { return true; }
  void poll() override {
    for (int f = 0; f < 4; ++f) cur[f] = base[f];
    last_ok_ = millis();
  }
  uint32_t pollPeriodMs() const override { return 200; }
  bool fresh() const override {
    return last_ok_ != 0 && millis() - last_ok_ < cfg::FRESH_WINDOW_MS;
  }
  uint32_t lastOkMs() const override { return last_ok_; }
  uint32_t failCount() const override { return fails_; }
  const FeatureProfile& profile() const override { return PROF; }
  void features(float* out) const override {
    // noise scales x4 vs test_host.cpp so 3-sigma level steps stay in the
    // range the bounded optimizer can track in test-wall time
    static const float scale[4] = {0.2f, 0.12f, 4.0f, 6.0f};
    for (int f = 0; f < 4; ++f) out[f] = cur[f] + noise(scale[f] * nscale);
    ++feeds;
  }
  size_t logLine(char* out, size_t cap) override {
    return snprintf(out, cap, "[FAKE] i=%.2f slip=%.2f vdc=%.0f tq=%.1f",
                    cur[0], cur[1], cur[2], cur[3]);
  }

private:
  // deterministic LCG noise (test_host.cpp pattern)
  mutable unsigned long lcg_ = 12345;
  float noise(float scale) const {
    lcg_ = lcg_ * 1664525u + 1013904223u;
    float u = (float)(lcg_ % 10000) / 10000.0f - 0.5f;
    return 2.0f * scale * u;
  }
  uint32_t last_ok_ = 0;
  uint32_t fails_ = 0;
};

// Drive loop() until the monitor has consumed n more feed steps.
static void runFeeds(SensorNodeApp& app, FakeSensor& s, long n) {
  const long start = s.feeds;
  long guard = 0;
  while (s.feeds - start < n) {
    app.tick();
    if (++guard > (n + 1000) * 500) {
      fprintf(stderr, "TRIPCHECK: feeds stalled (target %ld)\n", n);
      abort();
    }
  }
}

int main() {
  // capture the app's console output for segment scanning
  FILE* cap = freopen("/tmp/tinyrl_tripcheck.log", "w", stdout);
  assert(cap);

  FakeSensor sensor;
  SensorNodeApp app;
  NodeIdentity id{"fake-01", "v1/Plant01/Area/Line/Station-01",
                  "ssid", "pass", "127.0.0.1", 1883};
  app.begin(sensor, id);

  const float norm[4] = {3.0f, 0.25f, 585.0f, 45.0f};     // NORMAL (CAL mean)
  const float lvHi[4] = {3.35f, 0.46f, 585.0f, 55.4f};    // NORMAL + 3 sigma
  const float lvLo[4] = {2.65f, 0.04f, 585.0f, 34.6f};    // NORMAL - 3 sigma

  // ---- phase 0: learn NORMAL (full noise) ------------------------------
  Serial.feed("s\n");
  app.tick();
  runFeeds(app, sensor, 500);                    // CAL (300) + FIT (120) + margin

  // ---- step to lvHi (smooth) -> ALARM -> label -------------------------
  for (int f = 0; f < 4; ++f) { sensor.base[f] = lvHi[f]; }
  sensor.nscale = 0.2f;                          // steady (fault-like) regime
  runFeeds(app, sensor, 40);                     // alarm opens well within
  Serial.feed("f belt_slip\n");
  app.tick();                                    // library -> 1 condition

  // ---- stay at lvHi: adapt, clear, quiet steps MATCH centroid ----------
  runFeeds(app, sensor, 5000);
  Serial.feed("m\n");
  app.tick();
  fprintf(stdout, "\nPHASE2\n");

  // ---- step to lvLo: adapt, clear, quiet steps match NOTHING -----------
  for (int f = 0; f < 4; ++f) sensor.base[f] = lvLo[f];
  runFeeds(app, sensor, 9000);
  Serial.feed("m\n");
  app.tick();

  // ---- scan the app's own output ---------------------------------------
  fflush(stdout);
  fclose(stdout);
  FILE* f = fopen("/tmp/tinyrl_tripcheck.log", "r");
  assert(f);
  std::string log;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) log.append(buf, n);
  fclose(f);

  const size_t p2 = log.find("\nPHASE2\n");
  assert(p2 != std::string::npos);
  const std::string ph1 = log.substr(0, p2);
  const std::string ph2 = log.substr(p2);

  // sanity: the scenario really ran the learning/alarm/label path
  assert(ph1.find("learning STARTED") != std::string::npos);
  assert(ph1.find("[RL] ALARM") != std::string::npos);
  assert(ph1.find("learned new condition 'belt_slip'") != std::string::npos);
  // phase 1: matched quiet steps leave the counter at zero
  assert(ph1.find("quiet-unmatched=") == std::string::npos);
  assert(ph1.find("qum=0") != std::string::npos);
  // phase 2: unmatched quiet steps trip the counter and its report line
  assert(ph2.find("quiet-unmatched=") != std::string::npos);
  assert(ph2.find("qum=0") == std::string::npos);

  fprintf(stderr, "TRIPCHECK OK: tripwire silent on matched quiet steps, "
                  "fires off-centroid, local log only\n");
  return 0;
}
