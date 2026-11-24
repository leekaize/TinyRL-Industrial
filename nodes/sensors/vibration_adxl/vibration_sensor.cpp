// =====================================================================
// vibration_sensor.cpp -- 200 Hz sampling, DC-blocker, 2:1 decimation,
// 64-point FFT, rms/band/centroid features. The feature set and DSP
// parameters are locked from the clean rig capture (2026-11-22,
// tools/feature_selection.py; see vibration_sensor.h for the provenance
// and the retired 2026-11-21 lock); the FFT skeleton is the bench-proven
// MVP logic carried over from the 128-point era, resized.
// =====================================================================
#include "vibration_sensor.h"
#include "node_config.h"
#include <math.h>

bool VibrationSensor::begin() {
  for (int i = 0; i < FFT_N; i++)
    hann_[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / FFT_N));
  for (int i = 0; i < FFT_N; i++) {
    int r = 0, x = i;
    for (int b = 0; (1 << b) < FFT_N; b++) { r = (r << 1) | (x & 1); x >>= 1; }
    bitrev_[i] = (uint8_t)r;
  }
  for (int k = 0; k < FFT_N / 2; k++) {
    tw_r_[k] = cosf(-2.0f * (float)M_PI * k / FFT_N);
    tw_i_[k] = sinf(-2.0f * (float)M_PI * k / FFT_N);
  }

  bool ok = adxl345::init();
  Serial.printf("[VIB] ADXL345 %s | fs=200 dec2->%.0f Hz win=%.2f s | "
                "4 features: rms b2 rms_y centroid\n",
                ok ? "ready" : "NOT FOUND", FS_EFF, FFT_N / FS_EFF);
  return ok;
}

void VibrationSensor::toggleRawStream() {
  raw_ = !raw_;
  Serial.printf("[VIB] raw x,y,z stream %s (200 Hz, g) - "
                "tools/feature_selection.py --capture\n",
                raw_ ? "ON" : "OFF");
}

void VibrationSensor::poll() {
  // 200 Hz beat: independent of loop jitter, one sample per due slot.
  // The beat ANCHORS at the first poll (begin() runs seconds before the
  // loop starts: WiFi + FS mount would leave next_us_=0 vs a large
  // micros(), and the catch-up would then sample every call forever -
  // the 2026-11-21 capture ran at ~485 Hz because of exactly that).
  // A stall longer than 4 slots resyncs instead of bursting.
  uint32_t now_us = micros();
  if (next_us_ == 0) { next_us_ = now_us + TICK_US; return; }
  int32_t due = (int32_t)(now_us - next_us_);
  if (due < 0) return;
  if (due > (int32_t)(4 * TICK_US)) {
    next_us_ = now_us + TICK_US;
    return;
  }
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

  if (raw_)                        // raw stream: the UNBLOCKED frame, g
    Serial.printf("%.5f,%.5f,%.5f\n", x, y, z);

  // per-axis DC tracker (high-pass fc ~ 0.5 Hz): removes gravity + offset
  if (!have_hp_) { dev_x_ = x; dev_y_ = y; dev_z_ = z; have_hp_ = true; }
  float dx = x - dev_x_, dy = y - dev_y_, dz = z - dev_z_;
  dev_x_ += HP_ALPHA * (x - dev_x_);
  dev_y_ += HP_ALPHA * (y - dev_y_);
  dev_z_ += HP_ALPHA * (z - dev_z_);

  // 2:1 boxcar decimation (the study's decimate2): average consecutive
  // 200 Hz samples into one 100 Hz sample, then fill the 64-point window
  pair_[0] += dx; pair_[1] += dy; pair_[2] += dz;
  pair_hi_ = !pair_hi_;
  if (pair_hi_) return;            // first sample of the pair: keep summing
  float ax = pair_[0] * 0.5f, ay = pair_[1] * 0.5f, az = pair_[2] * 0.5f;
  pair_[0] = pair_[1] = pair_[2] = 0;
  wdev_[0][w_idx_] = ax;
  wdev_[1][w_idx_] = ay;
  wdev_[2][w_idx_] = az;
  wvib_[w_idx_] = sqrtf(ax * ax + ay * ay + az * az);
  if (++w_idx_ >= FFT_N) { w_idx_ = 0; processWindow(); }
}

void VibrationSensor::processWindow() {
  // amplitude features over the decimated window (mirror the study:
  // time_features() rms/rms_y from the DC-blocked frames; b2 and
  // centroid are bin sums over the spectrum built below)
  float sx = 0, sy = 0, wsum = 0;
  for (int i = 0; i < FFT_N; i++) {
    sx += wdev_[0][i] * wdev_[0][i];
    sy += wdev_[1][i] * wdev_[1][i];
    wsum += wvib_[i] * wvib_[i];
  }
  feat_[0] = sqrtf(wsum / FFT_N);                        // rms
  feat_[2] = sqrtf(sy / FFT_N);                          // rms_y

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
    for (int k = 1; k < FFT_N / 2; k++)      // skip DC and Nyquist (31 bins)
      binp_[k] += spec_re_[k] * spec_re_[k] + spec_im_[k] * spec_im_[k];
  }

  // b2: mean power over the 9-24 Hz band = bins 6..15 at 1.5625 Hz
  // (the study's BANDS b2 at n=64 fs=100), spectrum already summed
  // over the 3 axes
  float b2 = 0;
  for (int k = 6; k <= 15; k++) b2 += binp_[k];
  feat_[1] = b2 / 10.0f;
  // centroid: power-weighted mean bin frequency (k * fs_eff / FFT_N);
  // all-zero window reports 0 like the study's max(tot, 1e-30) path
  float tot = 0, wk = 0;
  for (int k = 1; k < FFT_N / 2; k++) { tot += binp_[k]; wk += binp_[k] * k; }
  feat_[3] = tot > 0 ? (wk / tot) * (FS_EFF / (float)FFT_N) : 0.0f;

  last_win_ms_ = millis();
}

bool VibrationSensor::fresh() const {
  return last_win_ms_ != 0 &&
         (millis() - last_win_ms_) < cfg::FRESH_WINDOW_MS;
}

void VibrationSensor::features(float* out) const {
  for (int f = 0; f < 4; ++f) out[f] = feat_[f];
}

size_t VibrationSensor::logLine(char* out, size_t cap) {
  return snprintf(out, cap,
    "[VIB] rms=%6.4f g | b2=%.3e g^2 | rms_y=%6.4f g | centroid=%6.1f Hz",
    feat_[0], feat_[1], feat_[2], feat_[3]);
}
