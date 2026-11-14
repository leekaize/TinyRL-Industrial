// =====================================================================
// vibration_sensor.cpp -- 200 Hz sampling, DC-blocker, FFT bands.
// The FFT + band math is bench-proven MVP logic (vibration_streamq),
// carried over verbatim; what changed is only the output: features are
// raw engineering units consumed by the sensor-agnostic core.
// =====================================================================
#include "vibration_sensor.h"
#include "node_config.h"
#include <math.h>

bool VibrationSensor::begin() {
  for (int i = 0; i < FFT_N; i++)
    hann_[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / FFT_N));
  for (int i = 0; i < FFT_N; i++) {
    int r = 0, x = i;
    for (int b = 0; b < 7; b++) { r = (r << 1) | (x & 1); x >>= 1; }
    bitrev_[i] = (uint8_t)r;
  }
  for (int k = 0; k < FFT_N / 2; k++) {
    tw_r_[k] = cosf(-2.0f * (float)M_PI * k / FFT_N);
    tw_i_[k] = sinf(-2.0f * (float)M_PI * k / FFT_N);
  }

  bool ok = adxl345::init();
  Serial.printf("[VIB] ADXL345 %s | fs=%.0f Hz win=%.2f s | 6 features: "
                "rms b1..b4 fmax\n",
                ok ? "ready" : "NOT FOUND", FS, FFT_N / FS);
  return ok;
}

void VibrationSensor::poll() {
  // 200 Hz beat: independent of loop jitter, one sample per due slot
  uint32_t now_us = micros();
  if ((int32_t)(now_us - next_us_) < (int32_t)TICK_US) return;
  next_us_ += TICK_US;

  float x, y, z;
  if (!adxl345::read3(x, y, z)) {
    poll_fails_++;
    if (++consec_fails_ >= FAIL_RECOVER) {
      consec_fails_ = 0;
      adxl345::recover();
    }
    return;
  }
  consec_fails_ = 0;

  // per-axis DC tracker (high-pass fc ~ 0.5 Hz): removes gravity + offset
  if (!have_hp_) { dev_x_ = x; dev_y_ = y; dev_z_ = z; have_hp_ = true; }
  float dx = x - dev_x_, dy = y - dev_y_, dz = z - dev_z_;
  dev_x_ += HP_ALPHA * (x - dev_x_);
  dev_y_ += HP_ALPHA * (y - dev_y_);
  dev_z_ += HP_ALPHA * (z - dev_z_);
  wdev_[0][w_idx_] = dx;
  wdev_[1][w_idx_] = dy;
  wdev_[2][w_idx_] = dz;
  wvib_[w_idx_] = sqrtf(dx * dx + dy * dy + dz * dz);
  if (++w_idx_ >= FFT_N) { w_idx_ = 0; processWindow(); }
}

void VibrationSensor::processWindow() {
  float rms = 0;
  for (int i = 0; i < FFT_N; i++) rms += wvib_[i] * wvib_[i];
  rms = sqrtf(rms / FFT_N);

  for (int k = 0; k < FFT_N / 2; k++) binp_[k] = 0;
  for (int ax = 0; ax < 3; ax++) {
    for (int i = 0; i < FFT_N; i++) {
      spec_re_[i] = wdev_[ax][i] * hann_[i];
      spec_im_[i] = 0;
    }
    // radix-2 in-place FFT (MVP-verbatim)
    for (int i = 0; i < FFT_N; i++) {
      int j = bitrev_[i];
      if (j > i) {
        float t = spec_re_[i]; spec_re_[i] = spec_re_[j]; spec_re_[j] = t;
        t = spec_im_[i]; spec_im_[i] = spec_im_[j]; spec_im_[j] = t;
      }
    }
    for (int len = 2; len <= FFT_N; len <<= 1) {
      int half = len >> 1, step = FFT_N / len;
      for (int i = 0; i < FFT_N; i += len) {
        for (int k = 0; k < half; k++) {
          int a = i + k, b = a + half, t = k * step;
          float wr = tw_r_[t], wi = tw_i_[t];
          float tr = spec_re_[b] * wr - spec_im_[b] * wi;
          float ti = spec_re_[b] * wi + spec_im_[b] * wr;
          spec_re_[b] = spec_re_[a] - tr; spec_im_[b] = spec_im_[a] - ti;
          spec_re_[a] += tr;             spec_im_[a] += ti;
        }
      }
    }
    for (int k = 1; k < FFT_N / 2; k++)      // skip DC
      binp_[k] += spec_re_[k] * spec_re_[k] + spec_im_[k] * spec_im_[k];
  }

  // band edges by FFT bin (bin width = 1.5625 Hz), 63 non-DC bins:
  //   b1: 1.6-7.8 Hz (5 bins)  b2: 9.4-23.4 (10)  b3: 25-54.7 (20)
  //   b4: 56.3-98.4 (28)
  float band[4] = {0, 0, 0, 0};
  int fmax_k = 1;
  float pmax = 0;
  for (int k = 1; k < FFT_N / 2; k++) {
    int b = k < 6 ? 0 : k < 16 ? 1 : k < 36 ? 2 : 3;
    band[b] += binp_[k];
    if (binp_[k] > pmax) { pmax = binp_[k]; fmax_k = k; }
  }
  static constexpr int BAND_NBINS[4] = {5, 10, 20, 28};
  for (int b = 0; b < 4; b++) band[b] /= BAND_NBINS[b];

  feat_[0] = rms;
  feat_[1] = band[0];
  feat_[2] = band[1];
  feat_[3] = band[2];
  feat_[4] = band[3];
  feat_[5] = fmax_k * FS / FFT_N;
  last_win_ms_ = millis();
}

bool VibrationSensor::fresh() const {
  return last_win_ms_ != 0 &&
         (millis() - last_win_ms_) < cfg::FRESH_WINDOW_MS;
}

void VibrationSensor::features(float* out) const {
  for (int f = 0; f < 6; ++f) out[f] = feat_[f];
}

size_t VibrationSensor::logLine(char* out, size_t cap) {
  return snprintf(out, cap,
    "[VIB] rms=%6.4f g | fmax=%6.1f Hz | bands %9.3g %9.3g %9.3g %9.3g g2",
    feat_[0], feat_[5], feat_[1], feat_[2], feat_[3], feat_[4]);
}
