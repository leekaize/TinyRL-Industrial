// Predictive-novelty vibration detection, ADXL345 (SPI default), FFT features.
//
// Sampling:  ODR 400 Hz, read at 200 Hz. Non-overlapping windows of 128
// samples (0.64 s). Per window: per-axis EMA high-pass (removes gravity),
// RMS of |a_HP|, 128-pt Hann FFT per axis -> 4 band-energy features +
// log(RMS)  =>  OBS_DIM 5. Observation = PREVIOUS window's features,
// target = log-bin of current RMS -> one-step predictor.
// Log-spaced bins: x1.5 amplitude per bin over [VIB_MIN, VIB_MAX] ->
// sensitive to mg-level changes, still valid to ~8 g.
// Phase CAL (AT REST): learns rest signature, sets theta. Phase MON: frozen;
// sustained prediction failure => ALARM -> LED.
// Serial: 'R' = recalibrate, 'P' = dump last window features.
//
// SPI wiring: VCC->3V3 GND->GND CS->GPIO5 SCL->SCK(18) SDA->MOSI(23) SDO->MISO(19)
// I2C wiring: VCC->3V3 GND->GND CS->3V3 SDO->GND SDA->GPIO21 SCL->GPIO22

#include <Arduino.h>

#ifndef USE_SPI
#define USE_SPI 1              // SPI proved robust near the motor; set 0 for I2C
#endif

#if USE_SPI
  #include <SPI.h>
#else
  #include <Wire.h>
#endif

#define AG_EMBEDDED 1
#include "matrix.h"
#include "autograd.h"
#include "layers.h"
#include "stream_q.h"

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ---------------- transport ----------------
#if USE_SPI
static const uint8_t  PIN_CS   = 5;
static const uint8_t  PIN_SCK  = 18;
static const uint8_t  PIN_MISO = 19;
static const uint8_t  PIN_MOSI = 23;
static const uint32_t SPI_HZ   = 1000000;
#else
static const uint8_t  ADXL_ADDR = 0x53;   // 0x1D if SDO high
static const uint32_t I2C_HZ    = 100000;
#endif

// ---------------- ADXL345 ----------------
static const uint32_t TICK_US      = 5000;  // 5 ms -> 200 Hz read rate
static const float    AXIS_CLIP    = 17.0f; // +-16 g config; beyond = corrupted frame
static const int      FAIL_RECOVER = 50;
static uint32_t next_us = 0;

// ---------------- windowing / FFT ----------------
static const int    FFT_N    = 128;        // 128 samples @ 200 Hz = 0.64 s window
static const float  FS       = 200.0f;
static const float  HP_ALPHA = 0.016f;     // per-sample DC tracker, fc ~ 0.5 Hz @ 200 Hz

// ---------------- detector ----------------
static const int   OBS_DIM      = 5;       // [logRMS, logB1..logB4]
static const int   N_ACTIONS    = 20;      // log-spaced bins over [VIB_MIN, VIB_MAX]
static const float VIB_MIN      = 0.002f;  // g; bottom of bin space
static const float VIB_MAX      = 8.0f;    // g; top of bin space
static const int   CALIB_STEPS  = 300;     // windows; ~3.2 min
static const int   TAIL_STEPS   = 120;     // threshold from converged tail only
static const int   HIDDEN       = 64;
static const float SCORE_ALPHA  = 0.12f;   // score EMA ~8 windows (~5 s)
static const int   ALARM_K      = 3;       // consecutive high-score windows => ALARM
static const int   CLEAR_M      = 40;      // quiet windows => clear (~26 s)

static StreamQ agent(OBS_DIM, N_ACTIONS, 1.0f, 0.99f, 0.8f, 2.0f);
static nn::Sequential qnet;

static char  line[32]; static size_t llen = 0;
static int   phase = 0;                  // 0=calibrate, 1=monitor
static long  step_n = 0;
static float score = 0.2f, theta = 0.0f;
static double s_sum = 0, s_sq = 0;
static int   hi = 0, lo = 0; static bool alarmed = false;
static long  tail_n = 0;

// detector state
static bool  have_prev = false;
static float prev_f[OBS_DIM];
static float last_rms = 0, last_fmax = 0; static int last_bin = 0;

// signal state
static bool  have_hp = false;
static float devX = 0, devY = 0, devZ = 0;
static int   w_idx = 0;

// calibration baseline (Welford over window RMS)
static float b_mu = 0, b_M2 = 0; static long b_n = 0;

// bus health
static int      bus_fail = 0;
static long     bus_ok = 0, bus_bad = 0;
static uint32_t last_recover_ms = 0;

// FFT workspace
static float hann_w[FFT_N];
static uint8_t bitrev[FFT_N];
static float tw_r[FFT_N/2], tw_i[FFT_N/2];
static float spec_re[FFT_N], spec_im[FFT_N];
static float binp[FFT_N/2];
static float wdev[3][FFT_N];             // per-sample HP deviation per axis
static float wvib[FFT_N];                // per-sample |a_HP|

// bands by FFT bin (bin width = 200/128 = 1.5625 Hz), bins 1..63
// B0: 1.6-7.8 Hz (5 bins)  B1: 9.4-23.4 Hz (10)  B2: 25-54.7 Hz (20)  B3: 56.3-98.4 Hz (28)
static const int BAND_NBINS[4] = {5, 10, 20, 28};
static inline int band_of(int k){ return k < 6 ? 0 : k < 16 ? 1 : k < 36 ? 2 : 3; }

static inline int vib_bin(float v){
  float x = (logf(fmaxf(v, VIB_MIN)) - logf(VIB_MIN)) / logf(VIB_MAX / VIB_MIN) * N_ACTIONS;
  int b = (int)x; return b < 0 ? 0 : (b >= N_ACTIONS ? N_ACTIONS - 1 : b);
}

// ---------------- ADXL345 low level ----------------
static const uint8_t REG_DEVID   = 0x00;  // expects 0xE5
static const uint8_t REG_BW_RATE = 0x2C;
static const uint8_t REG_PWR_CTL = 0x2D;
static const uint8_t REG_INT_EN  = 0x2E;
static const uint8_t REG_FORMAT  = 0x31;
static const uint8_t REG_DATAX0  = 0x32;

// Frame decoder + physical sanity check: with +-16 g configured and gravity
// always present, impossible values mean corrupted bytes -> reject.
static bool decode6(const uint8_t *b, float &x, float &y, float &z) {
  int16_t rx = (int16_t)(b[0] | (b[1] << 8));
  int16_t ry = (int16_t)(b[2] | (b[3] << 8));
  int16_t rz = (int16_t)(b[4] | (b[5] << 8));
  x = rx / 256.0f; y = ry / 256.0f; z = rz / 256.0f;  // FULL_RES: 3.9 mg/LSB
  if (fabsf(x) > AXIS_CLIP || fabsf(y) > AXIS_CLIP || fabsf(z) > AXIS_CLIP) return false;
  if (x*x + y*y + z*z < 0.0025f) return false;        // all-zero frame (|a| < 0.05 g)
  return true;
}

#if USE_SPI
static SPIClass adxlSPI(VSPI);

static void adxl_w(uint8_t reg, uint8_t val) {
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(reg);
  adxlSPI.transfer(val);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
}

static uint8_t adxl_r1(uint8_t reg) {
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(0x80 | reg);
  uint8_t v = adxlSPI.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
  return v;
}

static bool adxl_read3(float &x, float &y, float &z) {
  uint8_t b[6];
  adxlSPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE3));
  digitalWrite(PIN_CS, LOW);
  adxlSPI.transfer(0xC0 | REG_DATAX0);       // read + multibyte
  for (int i = 0; i < 6; i++) b[i] = adxlSPI.transfer(0x00);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.endTransaction();
  return decode6(b, x, y, z);
}

#else  // I2C

static bool adxl_w(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(reg); Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool adxl_read3(float &x, float &y, float &z) {
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL_ADDR, (uint8_t)6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  return decode6(b, x, y, z);
}
#endif

static bool adxl_init() {
#if USE_SPI
  pinMode(PIN_CS, OUTPUT);
  digitalWrite(PIN_CS, HIGH);
  adxlSPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  if (adxl_r1(REG_DEVID) != 0xE5) return false;
#else
  Wire.begin();
  Wire.setClock(I2C_HZ);
  Wire.beginTransmission(ADXL_ADDR);
  Wire.write(REG_DEVID);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)ADXL_ADDR, (uint8_t)1) != 1) return false;
  if (Wire.read() != 0xE5) return false;
#endif
  adxl_w(REG_PWR_CTL, 0x00);  // standby while configuring
  adxl_w(REG_BW_RATE, 0x0C);  // 400 Hz ODR: reading at 200 Hz always gets a fresh frame
  adxl_w(REG_FORMAT,  0x0B);  // FULL_RES, +-16 g
  adxl_w(REG_INT_EN,  0x00);  // no interrupts
  adxl_w(REG_PWR_CTL, 0x08);  // measure
  return true;
}

static void bus_recover() {
  Serial.println("[BUS] recovery: re-init transport + reconfigure ADXL345");
#if USE_SPI
  adxl_init();
#else
  Wire.end();
  delay(5);
  if (!adxl_init()) Serial.println("[BUS] recovery failed, sensor not responding");
#endif
}

// ---------------- FFT (radix-2, in-place, on spec_re/spec_im) ----------------
static void fft_run() {
  for (int i = 0; i < FFT_N; i++) {
    int j = bitrev[i];
    if (j > i) {
      float t = spec_re[i]; spec_re[i] = spec_re[j]; spec_re[j] = t;
      t = spec_im[i]; spec_im[i] = spec_im[j]; spec_im[j] = t;
    }
  }
  for (int len = 2; len <= FFT_N; len <<= 1) {
    int half = len >> 1, step = FFT_N / len;
    for (int i = 0; i < FFT_N; i += len) {
      for (int k = 0; k < half; k++) {
        int a = i + k, b = a + half, t = k * step;
        float wr = tw_r[t], wi = tw_i[t];
        float tr = spec_re[b]*wr - spec_im[b]*wi;
        float ti = spec_re[b]*wi + spec_im[b]*wr;
        spec_re[b] = spec_re[a] - tr; spec_im[b] = spec_im[a] - ti;
        spec_re[a] += tr;             spec_im[a] += ti;
      }
    }
  }
}

// ---------------- one decision per 0.64 s window ----------------
static void process_window() {
  double p = 0;
  for (int i = 0; i < FFT_N; i++) p += (double)wvib[i] * wvib[i];
  float rms = sqrtf((float)(p / FFT_N));

  for (int k = 0; k < FFT_N/2; k++) binp[k] = 0;
  for (int ax = 0; ax < 3; ax++) {
    for (int i = 0; i < FFT_N; i++) { spec_re[i] = wdev[ax][i] * hann_w[i]; spec_im[i] = 0; }
    fft_run();
    for (int k = 1; k < FFT_N/2; k++)        // skip DC
      binp[k] += spec_re[k]*spec_re[k] + spec_im[k]*spec_im[k];
  }
  float band[4] = {0, 0, 0, 0};
  int fmax_k = 1; float pmax = 0;
  for (int k = 1; k < FFT_N/2; k++) {
    band[band_of(k)] += binp[k];
    if (binp[k] > pmax) { pmax = binp[k]; fmax_k = k; }
  }
  for (int b = 0; b < 4; b++) band[b] /= BAND_NBINS[b];
  float fmax_hz = fmax_k * FS / FFT_N;

  float f[OBS_DIM] = { logf(rms + 1e-6f),
                       logf(band[0] + 1e-12f), logf(band[1] + 1e-12f),
                       logf(band[2] + 1e-12f), logf(band[3] + 1e-12f) };
  last_rms = rms; last_fmax = fmax_hz;

  if (!have_prev) { memcpy(prev_f, f, sizeof(prev_f)); have_prev = true; return; }
  int true_b = vib_bin(rms); last_bin = true_b;

  ag::Matrix raw(1, OBS_DIM);
  for (int j = 0; j < OBS_DIM; j++) raw(0, j) = prev_f[j];
  ag::Tensor s(agent.normalize_observation(raw), false);

  auto [act, is_ng] = agent.sample_action(s);
  int a = act;
  float d = fabsf((float)a - (float)true_b);
  float r = 1.0f - d / 4.0f; if (r < 0.0f) r = 0.0f;  // graded over 4 log bins

  if (phase == 0) {
    agent.update(s, act, agent.scale_reward(r, true), s, true, is_ng);
    if (step_n == CALIB_STEPS - TAIL_STEPS) { s_sum = s_sq = 0; tail_n = 0; }
    if (!is_ng) { double e = 1.0 - r; s_sum += e; s_sq += e*e; tail_n++; }
    b_n++;                                          // RMS baseline, all windows
    float dv = rms - b_mu; b_mu += dv / b_n; b_M2 += dv * (rms - b_mu);
    step_n++;
    if (step_n % 30 == 0)
      Serial.printf("[CAL] step=%ld r=%.2f rms=%.4f g bin=%d\n", step_n, r, rms, true_b);
    if (step_n >= CALIB_STEPS) {
      double n = tail_n > 0 ? tail_n : 1;
      double mu = s_sum / n, var = s_sq / n - mu*mu;
      theta = (float)(mu + 4.0 * sqrt(var > 0 ? var : 0));
      if (theta < 0.30f) theta = 0.30f;   // off-by-one bin (x1.5) stays quiet; >=x2.3 alarms
      if (theta > 0.55f) theta = 0.55f;
      phase = 1; score = (float)mu; step_n = 0;
      float bvar = b_n > 1 ? b_M2 / (b_n - 1) : 0.0f;
      Serial.printf("[ARM] theta=%.3f mu=%.3f rms_base=%.4f+-%.4f g -> MONITOR\n",
                    theta, mu, b_mu, sqrtf(bvar > 0 ? bvar : 0.0f));
    }
  } else {
    // ---- MONITOR: frozen predictor ----
    if (!is_ng) {
      score = (1.0f - SCORE_ALPHA) * score + SCORE_ALPHA * (1.0f - r);
      if (score > theta) { hi++; lo = 0; } else { lo++; if (lo > CLEAR_M) hi = 0; }
      if (!alarmed && hi >= ALARM_K) {
        alarmed = true; digitalWrite(LED_BUILTIN, HIGH);
        Serial.printf("[ALARM] step=%ld score=%.3f theta=%.3f rms=%.4f g fmax=%.1f Hz\n",
                      step_n, score, theta, rms, fmax_hz);
      } else if (alarmed && lo > CLEAR_M) {
        alarmed = false; digitalWrite(LED_BUILTIN, LOW);
        Serial.printf("[CLEAR] step=%ld score=%.3f rms=%.4f g\n", step_n, score, rms);
      }
    }
    step_n++;
    if (step_n % 30 == 0) {
      float bvar = b_n > 1 ? b_M2 / (b_n - 1) : 1.0f;
      float z = fabsf(rms - b_mu) / sqrtf(bvar > 1e-8f ? bvar : 1e-8f);
      Serial.printf("[MON] step=%ld rms=%.4f g z=%.1f fmax=%.1f Hz score=%.3f theta=%.3f heap=%d\n",
                    step_n, rms, z, fmax_hz, score, theta, (int)esp_get_free_heap_size());
    }
  }

  memcpy(prev_f, f, sizeof(prev_f));
}

static void collect_sample(float ax, float ay, float az) {
  if (!have_hp) { devX = ax; devY = ay; devZ = az; have_hp = true; }
  float dx = ax - devX, dy = ay - devY, dz = az - devZ;
  devX += HP_ALPHA * (ax - devX);
  devY += HP_ALPHA * (ay - devY);
  devZ += HP_ALPHA * (az - devZ);
  wdev[0][w_idx] = dx; wdev[1][w_idx] = dy; wdev[2][w_idx] = dz;
  wvib[w_idx] = sqrtf(dx*dx + dy*dy + dz*dz);
  if (++w_idx >= FFT_N) { w_idx = 0; process_window(); }
}

static void handle_cmd(const char* l) {
  if (l[0] == 'R') {
    phase = 0; step_n = 0; s_sum = s_sq = 0; tail_n = 0; hi = lo = 0; alarmed = false;
    b_mu = 0; b_M2 = 0; b_n = 0; have_prev = false; w_idx = 0;
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("[RECAL] keep the sensor at rest");
  } else if (l[0] == 'P') {
    Serial.printf("[VIB] rms=%.4f g bin=%d fmax=%.1f Hz | logRMS=%.2f logB=%.2f/%.2f/%.2f/%.2f\n",
                  last_rms, last_bin, last_fmax,
                  prev_f[0], prev_f[1], prev_f[2], prev_f[3], prev_f[4]);
  }
}

void setup() {
  Serial.begin(115200);
  ag::manual_seed(123);
  qnet.add(nn::Linear(OBS_DIM, HIDDEN));
  qnet.add(nn::ReLU());
  qnet.add(nn::Linear(HIDDEN, N_ACTIONS));
  agent.set_model(qnet);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  for (int i = 0; i < FFT_N; i++)
    hann_w[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / FFT_N));
  for (int i = 0; i < FFT_N; i++) {
    int r = 0, x = i;
    for (int b = 0; b < 7; b++) { r = (r << 1) | (x & 1); x >>= 1; }
    bitrev[i] = (uint8_t)r;
  }
  for (int k = 0; k < FFT_N/2; k++) {
    tw_r[k] = cosf(-2.0f * (float)M_PI * k / FFT_N);
    tw_i[k] = sinf(-2.0f * (float)M_PI * k / FFT_N);
  }

  if (!adxl_init()) {
    Serial.println("[ERR] ADXL345 not found (check wiring). Halting.");
    while (true) { digitalWrite(LED_BUILTIN, HIGH); delay(80); digitalWrite(LED_BUILTIN, LOW); delay(120); }
  }
  Serial.printf("[BOOT] vib-streamq FFT OBS=%d ACTS=%d CAL=%d win=%.2fs fs=%.0fHz transport=%s\n",
                OBS_DIM, N_ACTIONS, CALIB_STEPS, FFT_N / FS, FS, USE_SPI ? "SPI" : "I2C");
  Serial.println("[CAL] keep the sensor AT REST until [ARM]");
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { line[llen] = '\0'; handle_cmd(line); llen = 0; }
    else if (c != '\r' && llen < sizeof(line)-1) line[llen++] = c;
  }

  uint32_t now = micros();
  if ((int32_t)(now - next_us) >= 0) {
    if ((int32_t)(now - next_us) > (int32_t)TICK_US) next_us = now;
    next_us += TICK_US;

    float x, y, z;
    bool ok = adxl_read3(x, y, z) || adxl_read3(x, y, z);
    if (ok) {
      bus_fail = 0; bus_ok++;
      collect_sample(x, y, z);
    } else {
      bus_fail++; bus_bad++;
      if (bus_fail == 1 || bus_fail == 10 || bus_fail % 100 == 0)
        Serial.printf("[BUS] %d consecutive bad reads (%ld ok / %ld bad)\n", bus_fail, bus_ok, bus_bad);
      if (bus_fail >= FAIL_RECOVER) {
        uint32_t ms = millis();
        if (ms - last_recover_ms > 200) { bus_recover(); last_recover_ms = ms; }
        bus_fail = 0;
      }
    }
  }
}
