// =====================================================================
// persistence.cpp -- v4 model snapshot + TinyRL weight blob I/O.
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "persistence.h"
#include "condition_library.h"

// ---- POD mirror of TinyRLConditionMonitor::State (fixed layout) -------
struct ModelBlobV4 {
  uint32_t magic, version;
  float    score, fit_mu, fit_m2, theta;
  uint32_t step;
  uint8_t  cal, theta_ready, alarm;
  int16_t  hi, lo;
  float    fs_lo[cfg::MAX_FEATURES], fs_hi[cfg::MAX_FEATURES];
  uint8_t  fs_armed;
  uint32_t fs_steps;
  float    nm_mean[cfg::MAX_FEATURES], nm_var[cfg::MAX_FEATURES];
};

namespace persist {

bool beginFS() {
  if (LittleFS.begin(false)) return true;
  Serial.println("[FS] mount failed - formatting");
  return LittleFS.begin(true);
}

bool modelSave(const TinyRLConditionMonitor& m) {
  TinyRLConditionMonitor::State s;
  m.exportState(s);
  ModelBlobV4 b{};
  b.magic = cfg::MODEL_MAGIC; b.version = cfg::MODEL_VERSION;
  b.score = s.score; b.fit_mu = s.fit_mu; b.fit_m2 = s.fit_m2; b.theta = s.theta;
  b.step = s.step;
  b.cal = s.cal; b.theta_ready = s.theta_ready; b.alarm = s.alarm;
  b.hi = s.hi; b.lo = s.lo;
  memcpy(b.fs_lo, s.fs_lo, sizeof(b.fs_lo));
  memcpy(b.fs_hi, s.fs_hi, sizeof(b.fs_hi));
  b.fs_armed = s.fs_armed; b.fs_steps = s.fs_steps;
  memcpy(b.nm_mean, s.nm_mean, sizeof(b.nm_mean));
  memcpy(b.nm_var,  s.nm_var,  sizeof(b.nm_var));

  File f = LittleFS.open(cfg::MODEL_FILE, "w");
  if (!f) { Serial.println("[FS] model.bin write FAILED"); return false; }
  f.write((uint8_t*)&b, sizeof(b));
  f.close();
  return true;
}

bool modelLoad(TinyRLConditionMonitor& m) {
  if (!LittleFS.exists(cfg::MODEL_FILE)) return false;
  File f = LittleFS.open(cfg::MODEL_FILE, "r");
  if (!f) return false;
  ModelBlobV4 b{};
  size_t n = f.read((uint8_t*)&b, sizeof(b));
  f.close();
  if (n != sizeof(b) || b.magic != cfg::MODEL_MAGIC ||
      b.version != cfg::MODEL_VERSION)
    return false;
  TinyRLConditionMonitor::State s{};
  s.score = b.score; s.fit_mu = b.fit_mu; s.fit_m2 = b.fit_m2; s.theta = b.theta;
  s.step = b.step;
  s.cal = b.cal; s.theta_ready = b.theta_ready; s.alarm = b.alarm;
  s.hi = b.hi; s.lo = b.lo;
  memcpy(s.fs_lo, b.fs_lo, sizeof(s.fs_lo));
  memcpy(s.fs_hi, b.fs_hi, sizeof(s.fs_hi));
  s.fs_armed = b.fs_armed; s.fs_steps = b.fs_steps;
  memcpy(s.nm_mean, b.nm_mean, sizeof(s.nm_mean));
  memcpy(s.nm_var,  b.nm_var,  sizeof(s.nm_var));
  return m.importState(s);
}

// ---- TinyRL network serialization -------------------------------------
// Walk nn::Sequential; every nn::Linear layer contributes its weights and
// bias verbatim (shape + data). Topology is re-validated on load.

bool netSave(const TinyRLConditionMonitor& m) {
  File f = LittleFS.open(cfg::NET_FILE, "w");
  if (!f) { Serial.println("[FS] net.bin write FAILED"); return false; }
  uint32_t hdr[4] = { cfg::NET_MAGIC, cfg::NET_VERSION,
                      (uint32_t)m.net().size(), 0 };
  const auto& layers = m.net().layers();
  for (auto& l : layers)
    if (l->has_parameters()) hdr[3]++;
  f.write((uint8_t*)hdr, sizeof(hdr));

  bool ok = true;
  for (auto& l : layers) {
    nn::Linear* lin = (nn::Linear*)l.get();     // topology is fixed in core
    if (!l->has_parameters()) continue;
    const ag::Matrix& w = lin->weights.value();
    const ag::Matrix& b = lin->bias.value();
    if (f.write((uint8_t*)w.shape().data(), sizeof(ag::Matrix::Shape)) !=
        sizeof(ag::Matrix::Shape)) { ok = false; break; }
    if (f.write((uint8_t*)w.data(), w.size() * sizeof(float)) !=
        w.size() * sizeof(float)) { ok = false; break; }
    if (f.write((uint8_t*)b.shape().data(), sizeof(ag::Matrix::Shape)) !=
        sizeof(ag::Matrix::Shape)) { ok = false; break; }
    if (f.write((uint8_t*)b.data(), b.size() * sizeof(float)) !=
        b.size() * sizeof(float)) { ok = false; break; }
  }
  f.close();
  if (!ok) Serial.println("[FS] net.bin write incomplete");
  return ok;
}

bool netLoad(TinyRLConditionMonitor& m) {
  if (!LittleFS.exists(cfg::NET_FILE)) return false;
  File f = LittleFS.open(cfg::NET_FILE, "r");
  if (!f) return false;
  uint32_t hdr[4] = {0, 0, 0, 0};
  if (f.read((uint8_t*)hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr[0] != cfg::NET_MAGIC || hdr[1] != cfg::NET_VERSION ||
      hdr[2] != (uint32_t)m.net().size()) {
    f.close(); return false;
  }
  const auto& layers = m.net().layers();
  uint32_t n_param_layers = 0;
  for (auto& l : layers)
    if (l->has_parameters()) n_param_layers++;
  if (hdr[3] != n_param_layers) { f.close(); return false; }

  bool ok = true;
  for (auto& l : layers) {
    if (!l->has_parameters()) continue;
    nn::Linear* lin = (nn::Linear*)l.get();
    ag::Matrix::Shape sh;
    if (f.read((uint8_t*)sh.data(), sizeof(sh)) != sizeof(sh)) { ok = false; break; }
    ag::Matrix w(sh);
    if ((int)w.numel() != (int)lin->weights.value().numel() ||
        f.read((uint8_t*)w.data(), w.size() * sizeof(float)) !=
        w.size() * sizeof(float)) { ok = false; break; }
    if (f.read((uint8_t*)sh.data(), sizeof(sh)) != sizeof(sh)) { ok = false; break; }
    ag::Matrix b(sh);
    if ((int)b.numel() != (int)lin->bias.value().numel() ||
        f.read((uint8_t*)b.data(), b.size() * sizeof(float)) !=
        b.size() * sizeof(float)) { ok = false; break; }
    lin->weights.set_value(w);
    lin->bias.set_value(b);
  }
  f.close();
  if (!ok) Serial.println("[FS] net.bin corrupt - starting fresh");
  return ok;
}

void factoryReset() {
  LittleFS.remove(cfg::MODEL_FILE);
  LittleFS.remove(cfg::NET_FILE);
  LittleFS.remove(cfg::COND_FILE);
  Serial.println("[FS] FACTORY RESET done - model + net + library erased");
}

} // namespace persist
