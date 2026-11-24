// =====================================================================
// node_runtime.cpp -- orchestration preserved from the vfd_streamq MVP,
// generalized over the FeatureProfile (nothing here knows about VFDs).
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include "node_runtime.h"
#include "persistence.h"
#include "json_mini.h"

// ---------------- helpers --------------------------------------------
static void slugify(const char* in, char* out, size_t len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o < len - 1; i++) {
    char c = in[i];
    if (isalnum((int)c)) out[o++] = (char)tolower((int)c);
    else if (o == 0 || out[o - 1] != '_') out[o++] = '_';
  }
  while (o > 0 && out[o - 1] == '_') o--;
  out[o] = 0;
}

const char* SensorNodeApp::conditionId() const {
  if (!mon_.thetaReady()) return mon_.learning() ? "learning" : "idle";
  if (mon_.alarmed()) {
    if (ep_.isOpen()) {
      // "unknown" IS the ask (README contract): it may only go out once
      // the episode holds a matured settled batch. Before that the state
      // is "unidentified" - alarmed, batch maturing, no upstream call.
      if (strcmp(ep_.cond(), "unknown") == 0 &&
          ep_.count() < cfg::ASK_MIN_BATCH) return "unidentified";
      return ep_.cond();
    }
    return "unknown";
  }
  // quiet: surface the recognised state, ANY kind (user decision
  // 2026-11-15: a learned fault shows as recognised while quiet - the
  // net predicts it, so it no longer alarms; the name is the evidence)
  int m = lib_.match(last_raw_, mon_.space());
  if (m >= 0) return lib_.at(m).id;
  return "normal";
}

float SensorNodeApp::score() const { return mon_.score(); }
float SensorNodeApp::theta() const { return mon_.theta(); }

const char* SensorNodeApp::phase() const {
  if (!mon_.learning() && !mon_.thetaReady()) return "idle";
  if (!mon_.thetaReady()) return mon_.space().armed() ? "fit" : "cal";
  return mon_.alarmed() ? "alarm" : "mon";
}

bool SensorNodeApp::episodeOpen() const { return ep_.isOpen(); }
uint32_t SensorNodeApp::stepCount() const { return mon_.steps(); }
uint32_t SensorNodeApp::libSize() const { return lib_.size(); }

float SensorNodeApp::gate() const { return last_gate_; }
float SensorNodeApp::ragg() const { return last_ragg_; }
float SensorNodeApp::gateR() const { return last_gate_r_; }
float SensorNodeApp::acc() const { return last_acc_; }

const char* SensorNodeApp::libName(int idx) const {
  return (idx >= 0 && idx < lib_.size()) ? lib_.at(idx).id : "";
}

void SensorNodeApp::publishCondition(bool retained) {
  uint32_t now = millis();
  msg_.publishCondition(mon_.stateString(), conditionId(),
                        mon_.confidence(), ep_.duration(now), retained);
}

void SensorNodeApp::saveAll() {
  persist::modelSave(mon_);
  persist::netSave(mon_);
  last_model_save_ = millis();
}

void SensorNodeApp::onEvent(const char* ev, const char* sev, const char* cid,
                            uint32_t dur, const char* summary,
                            const float* means) {
  msg_.publishEvent(ev, sev, cid, dur, summary, means);
}

// ---------------- lifecycle ------------------------------------------
void SensorNodeApp::begin(ISensorNode& sensor, const NodeIdentity& id) {
  sensor_ = &sensor;
  dev_id_ = id.dev_id;
  last_cond_key_[0] = 0;
  last_model_save_ = millis();
  quiet_unmatched_ = 0;

  sensor.begin();

  persist::beginFS();
  mon_.begin(sensor.profile());
  mon_.setNodeTag(id.dev_id);   // diagnostics: [WHY]/[RL] lines name their node

  bool restored = persist::modelLoad(mon_);
  bool net_ok = false;
  if (restored && mon_.thetaReady()) {
    net_ok = persist::netLoad(mon_);
    if (!net_ok) {                       // theta without weights = broken pair
      Serial.println("[FS] theta found but network missing/corrupt - relearning");
      mon_.factoryReset();
      restored = false;
    }
  }

  lib_.begin(sensor.profile().n_features);
  lib_.load(cfg::COND_FILE);
  mon_.attachLibrary(&lib_);   // normal-kind centroids quiet the alarm

  pend_.begin(sensor.profile().n_features);
  pend_.load(cfg::PENDING_FILE);   // stored unknown batches survive reboot

  const FeatureProfile& prof = sensor.profile();
  ep_.begin(&prof, &lib_, dev_id_,
            [this](const char* ev, const char* sev, const char* cid,
                   uint32_t dur, const char* sum, const float* means) {
              onEvent(ev, sev, cid, dur, sum, means);
            });
  msg_.begin(id, prof, [this](const char* j) { applyFeedback(j); });
  con_.begin(this);

  // boot report (v3 semantics)
  if (restored && mon_.thetaReady()) {
    Serial.printf("[FS] TinyRL model+net restored (theta=%.2f) - monitoring, no relearn\n",
                  mon_.theta());
  } else if (restored && mon_.learning()) {
    Serial.println("[FS] learning was in progress - CAL restarts automatically");
  } else {
    Serial.println("[FS] fresh model - IDLE. Press 's' (or MQTT {\"action\":\"start\"}) "
                   "to start learning at NORMAL load");
  }
  Serial.printf("[FS] %d known condition(s)\n", lib_.size());
  Serial.printf("[FS] %d pending batch(es)\n", pend_.size());

  // machine-readable capture header: the serial logger and the offline
  // replay tool parse this once, then stream [RAW] lines (see tick())
  char hdr[192];
  int o = snprintf(hdr, sizeof(hdr),
                   "[HDR] dev=%s type=%s fw=%s n=%d features=",
                   id.dev_id, prof.node_type, cfg::FW_VERSION, prof.n_features);
  for (int f = 0; f < prof.n_features && o > 0 && o < (int)sizeof(hdr) - 24; ++f) {
    o += snprintf(hdr + o, sizeof(hdr) - o, "%s%s:%s", f ? "," : "",
                  prof.features[f].name, prof.features[f].unit);
  }
  Serial.println(hdr);
}

void SensorNodeApp::startLearning() {
  if (!mon_.thetaReady()) {
    mon_.startLearning();
    saveAll();
    publishCondition(true);
  } else {
    mon_.startLearning();                // prints the "already monitoring" hint
  }
}

// ---------------- feedback (Action/feedback + console 'f') -----------
void SensorNodeApp::applyFeedback(const char* j) {
  char action[16] = "", label[32] = "", by[20] = "", kindstr[8] = "";
  char batch[12] = "";
  jsonStr(j, "action", action, sizeof(action));
  jsonStr(j, "label",  label,  sizeof(label));
  jsonStr(j, "by",     by,     sizeof(by));
  jsonStr(j, "kind",   kindstr, sizeof(kindstr));
  jsonStr(j, "batch",  batch,  sizeof(batch));
  // kind defaults to FAULT: conservative - only an explicitly named normal
  // condition ever auto-accepts into the alarm baseline
  CondKind kind = (strcmp(kindstr, "normal") == 0) ? COND_NORMAL : COND_FAULT;
  Serial.printf("[FB] action='%s' label='%s' kind='%s' by='%s'\n",
                action, label, kindstr, by);

  if (strcmp(action, "start") == 0) { startLearning(); return; }

  if (strcmp(action, "label") == 0 && label[0]) {
    if (batch[0]) {
      // batch flow (user workflow 2026-11-15): learn from the STORED
      // settled batch - works long after the episode closed
      int pi = pend_.find(batch);
      if (pi < 0) {
        Serial.printf("[FB] no pending batch '%s'\n", batch);
        return;
      }
      char slug[24];
      slugify(label, slug, sizeof(slug));
      bool existed = lib_.has(slug);
      int idx = lib_.label(slug, pend_.at(pi).rmean, pend_.at(pi).rstd,
                           by, kind, &mon_.space());
      if (idx < 0) {
        Serial.println("[FB] condition library full (LIB_MAX)");
        return;
      }
      pend_.remove(batch);
      pend_.save(cfg::PENDING_FILE);
      ep_pend_ = -1;                 // the episode's batch identity is gone
      if (ep_.isOpen())
        ep_.relabel(slug, kind, millis());   // the operator named the
                                             // state the node is IN
      mon_.voidPosEvidence();        // the case is closed: a stale pos_acc_
                                     // reservoir must not re-fire borderline
                                     // moments after the label (run-120222)
      // governance: a fault label sharpens; mapping a batch onto an
      // EXISTING known state desensitises (user point 6). A NEW normal
      // condition needs no theta change: the net now trains on it and
      // surprise falls on its own.
      if (kind == COND_FAULT) {
        mon_.onFeedbackFault();
        Serial.printf("[FB] theta lowered to %.2f\n", mon_.theta());
      } else if (existed) {
        mon_.onFeedbackNormal();
        Serial.printf("[FB] theta raised to %.2f\n", mon_.theta());
      }
      lib_.save(cfg::COND_FILE);
      saveAll();
      publishCondition(true);
      Serial.printf("[FB] batch %s labeled '%s' (%s)\n", batch, slug,
                    kindstr);
      return;
    }
    // legacy live-episode flow: the ask is being answered right now
    if (!ep_.isOpen()) { Serial.println("[FB] no open episode to label"); return; }
    char slug[24];
    slugify(label, slug, sizeof(slug));
    float rmean[cfg::MAX_FEATURES], rstd[cfg::MAX_FEATURES];
    ep_.relMeans(rmean);
    ep_.relStd(rstd);
    int idx = lib_.label(slug, rmean, rstd, by, kind, &mon_.space());
    if (idx < 0) {
      Serial.println("[FB] condition library full (LIB_MAX)");
      return;
    }
    ep_.relabel(slug, kind, millis());
    mon_.voidPosEvidence();          // the answered ask closes its own case
    if (ep_pend_ >= 0) {             // the answered episode's stored batch
      char bid[12];                  // is consumed with it: no orphan batch
      snprintf(bid, sizeof(bid), "%s", pend_.at(ep_pend_).id);
      pend_.remove(bid);
      pend_.save(cfg::PENDING_FILE);
      ep_pend_ = -1;
    }
    if (kind == COND_FAULT)
      mon_.onFeedbackFault();        // confirmed fault -> sharpen (plan.md:
                                     // normal +10% / fault -5% governance)
    else
      mon_.onWorldChange();          // regime changed -> drop traces
    lib_.save(cfg::COND_FILE);
    saveAll();
    publishCondition(true);
    return;
  }

  if (strcmp(action, "discard") == 0 && batch[0]) {
    // discard deletes the batch only: the model is untouched, so the
    // same unknown will ask again on its next occurrence (user point 6).
    // The OPEN episode must not re-ask its own occurrence: dismissing
    // arms ep_ask_dismissed_ until a new episode opens
    if (pend_.remove(batch)) {
      pend_.save(cfg::PENDING_FILE);
      if (ep_pend_ >= 0 && ep_.isOpen() &&
          strcmp(ep_.cond(), "unknown") == 0)
        ep_ask_dismissed_ = true;
      ep_pend_ = -1;
      Serial.printf("[FB] batch %s discarded (model untouched)\n", batch);
    } else {
      Serial.printf("[FB] no pending batch '%s'\n", batch);
    }
    publishCondition(true);
    return;
  }

  if (strcmp(action, "normal") == 0) {   // false alarm -> desensitise
    mon_.onFeedbackNormal();
    ep_.close("normal", mon_.space(), millis());
    saveAll();
    publishCondition(true);
    Serial.printf("[FB] theta raised to %.2f\n", mon_.theta());
    return;
  }

  if (strcmp(action, "fault") == 0) {    // confirmed -> sharpen
    mon_.onFeedbackFault();
    saveAll();
    publishCondition(true);
    Serial.printf("[FB] theta lowered to %.2f\n", mon_.theta());
    return;
  }
  // unknown action: ignored
}

// ---------------- console / maintenance commands ---------------------
void SensorNodeApp::sensorPollNow() {
  sensor_->poll();
  char line[160];
  sensor_->logLine(line, sizeof(line));
  Serial.println(line);
}

void SensorNodeApp::listLibrary() { lib_.list(); }

void SensorNodeApp::listPending() { pend_.list(); }

bool SensorNodeApp::batchMoved(const float* raw) const {
  if (ep_pend_ < 0) return false;
  const PendingBatch& b = pend_.at(ep_pend_);
  for (int f = 0; f < mon_.space().nFeatures(); ++f) {
    float unit = fmaxf(b.rstd[f], 1e-3f);
    if (fabsf(mon_.space().rel(f, raw[f]) - b.rmean[f])
        > cfg::QUIET_Q * unit)
      return true;
  }
  return false;
}

// Ask policy (user decision 2026-11-19): the ask (= stored batch +
// condition "unknown") fires only when the open episode holds
// ASK_MIN_BATCH settled samples. Until then the state publishes
// "unidentified" and keeps accumulating; a state that resolves into a
// known condition re-identifies (the settled identity check in tick())
// and never asks at all.
void SensorNodeApp::askIfMature(uint32_t now) {
  if (ep_pend_ >= 0 || ep_ask_dismissed_ || !ep_.isOpen() ||
      strcmp(ep_.cond(), "unknown") != 0 ||
      ep_.count() < cfg::ASK_MIN_BATCH) return;
  float rm[cfg::MAX_FEATURES], rs[cfg::MAX_FEATURES];
  ep_.relMeans(rm);
  ep_.relStd(rs);
  int pi = pend_.add(rm, rs, ep_.count(), now, mon_.steps());
  if (pi >= 0) {
    ep_pend_ = pi;
    Serial.printf("[PEND] batch %s stored (%lu samples) - asking: settled"
                  " samples reached ASK_MIN_BATCH %d. Label it by id:"
                  " 'f %s <label> [normal|fault]'\n",
                  pend_.at(pi).id, (unsigned long)pend_.at(pi).n,
                  cfg::ASK_MIN_BATCH, pend_.at(pi).id);
    pend_.save(cfg::PENDING_FILE);
    publishCondition(true);       // condition flips unidentified -> unknown
  }
}

uint32_t SensorNodeApp::pendingSize() const { return pend_.size(); }

bool SensorNodeApp::pendingAt(int i, char* id, size_t cap,
                              uint32_t* n, uint32_t* step) const {
  if (i < 0 || i >= (int)pend_.size()) return false;
  snprintf(id, cap, "%s", pend_.at(i).id);
  if (n) *n = pend_.at(i).n;
  if (step) *step = pend_.at(i).last_step;
  return true;
}

bool SensorNodeApp::libCentroid(int idx, float* out, CondKind* kind) const {
  if (idx < 0 || idx >= lib_.size()) return false;
  const Condition& c = lib_.at(idx);
  for (int f = 0; f < mon_.space().nFeatures(); ++f) out[f] = c.c[f];
  if (kind) *kind = c.kind;
  return true;
}

float SensorNodeApp::libUnit(int idx, int f) const {
  if (idx < 0 || idx >= lib_.size() || f < 0 || f >= mon_.space().nFeatures())
    return 0.0f;
  return lib_.at(idx).u[f];
}

float SensorNodeApp::libTheta(int idx) const {
  // per-condition score threshold; -1 = no condition (the caller falls
  // back to the global theta)
  if (idx < 0 || idx >= lib_.size()) return -1.0f;
  return lib_.scoreTheta(idx, mon_.theta());
}

float SensorNodeApp::fsAnchor(int f) const {
  return (f >= 0 && f < mon_.space().nFeatures()) ? mon_.space().anchor(f)
                                                  : 0.0f;
}

void SensorNodeApp::printStatus() {
  char line[160];
  sensor_->logLine(line, sizeof(line));
  Serial.printf("[NODE] %s | state=%s cond=%s conf=%.2f\n",
                line, mon_.stateString(), conditionId(), mon_.confidence());
  Serial.printf("%s[RL] step=%lu score=%.3f theta=%.3f params=%u lib=%d pubs=%lu "
                "qum=%lu heap=%u\n", dev_id_,
                (unsigned long)mon_.steps(), mon_.score(), mon_.theta(),
                (unsigned)mon_.paramCount(), lib_.size(),
                (unsigned long)msg_.publishes(), (unsigned long)quiet_unmatched_,
                (unsigned)ESP.getFreeHeap());
}

void SensorNodeApp::toggleVerbose() {
  sensor_verbose_ = !sensor_verbose_;
  sensor_->setVerbose(sensor_verbose_);
  Serial.printf("[NODE] sensor verbose %s\n", sensor_verbose_ ? "ON" : "OFF");
}

void SensorNodeApp::toggleTrace() {
  trace_ = !trace_;
  Serial.printf("[NODE] decision trace %s - every monitor step now prints "
                "the model internals (x, rel, resid, match, score/gates, "
                "evidence, training)\n", trace_ ? "ON" : "OFF");
}

void SensorNodeApp::toggleRawStream() {
  sensor_->toggleRawStream();     // the sensor acks itself (it knows the
}                                 // sample rate and line format)

void SensorNodeApp::factoryReset() {
  mon_.factoryReset();
  lib_.clear();
  ep_.drop();            // an open episode must not survive the wipe (it
  ep_pend_ = -1;         // re-surfaced as a stale 'superseded' close)
  ep_ask_dismissed_ = false;
  pend_.begin(sensor_->profile().n_features);
  LittleFS.remove(cfg::PENDING_FILE);
  persist::factoryReset();
  last_cond_key_[0] = 0;
  quiet_unmatched_ = 0;
  ident_drift_ = 0;
  baseline_registered_ = false;
  Serial.println("[FS] back to IDLE - press 's' to start learning again");
}

// ---------------- main tick ------------------------------------------
void SensorNodeApp::tick() {
  con_.tick();
  msg_.maintain();

  uint32_t now = millis();
  static uint32_t t_next_poll = 0, t_next_print = 0, t_next_feed = 0;
  bool fresh = sensor_->fresh();

  if ((int32_t)(now - t_next_poll) >= 0) {
    t_next_poll = now + sensor_->pollPeriodMs();
    sensor_->poll();
  }

  if ((int32_t)(now - t_next_print) >= 0) {
    t_next_print = now + cfg::PRINT_PERIOD_MS;
    char line[160];
    if (fresh) {
      sensor_->logLine(line, sizeof(line));
      Serial.println(line);
      // machine line: features in profile() order (the identity that indexes
      // the model), plus the decision context. The capture tool parses this;
      // logLine() above stays the human view.
      float vals[cfg::MAX_FEATURES];
      sensor_->features(vals);
      const FeatureProfile& prof = sensor_->profile();
      char raw[320];
      int o = snprintf(raw, sizeof(raw), "[RAW] ts_ms=%lu", (unsigned long)now);
      for (int f = 0; f < prof.n_features && o > 0 && o < (int)sizeof(raw) - 56; ++f) {
        o += snprintf(raw + o, sizeof(raw) - o, " %s=%.6g",
                      prof.features[f].name, (double)vals[f]);
      }
      snprintf(raw + o, sizeof(raw) - o,
               " state=%s cond=%s conf=%.2f score=%.3f theta=%.3f"
               " rag=%.3f gate_r=%.3f acc=%.2f posref=%s",
               mon_.stateString(), conditionId(), mon_.confidence(),
               mon_.score(), mon_.theta(),
               (double)last_ragg_, (double)last_gate_r_, (double)last_acc_,
               (last_posref_ >= 0 && last_posref_ < (int)lib_.size())
                   ? lib_.at(last_posref_).id : "-");
      Serial.println(raw);
    } else {
      Serial.printf("[NODE] no fresh data (failed polls=%lu) - check wiring/bus master\n",
                    (unsigned long)sensor_->failCount());
    }
  }

  if ((int32_t)(now - t_next_feed) >= 0) {
    t_next_feed = now + cfg::FEED_PERIOD_MS;
    if (fresh && (mon_.learning() || mon_.thetaReady())) {
      sensor_->features(last_raw_);
      have_raw_ = true;
      TinyRLConditionMonitor::StepOut out;
      mon_.step(last_raw_, out);
      last_gate_ = out.gate;              // bench figures: the evidence
      last_ragg_ = out.ragg;              // triple (active gate, positional
      last_gate_r_ = out.gate_r;          // band, accumulator) from THIS
      last_acc_ = out.acc;                // step, read via gate()/acc()
      last_posref_ = out.pos_ref;         // the band the positional evidence
                                          // was judged against (-1 = none)

      // per-step decision trace (console 'd'): every quantity that drove
      // this step, so each alarm/clear/ask is explainable from the log
      if (trace_) {
        const FeatureProfile& prof = sensor_->profile();
        float rel[cfg::MAX_FEATURES], resid[cfg::MAX_FEATURES];
        for (int f = 0; f < prof.n_features; ++f)
          rel[f] = mon_.space().rel(f, last_raw_[f]);
        int fr = out.frame;
        if (fr >= 0 && fr < lib_.size()) {
          const Condition& fc = lib_.at(fr);
          for (int f = 0; f < prof.n_features; ++f) {
            float unit = fc.u[f] > 0 ? fc.u[f] : mon_.space().unit(f);
            resid[f] = (rel[f] - fc.c[f]) / unit;   // the net's input dim
          }
        }
        Serial.printf("[ST %lu] settled=%d grace=%d sticky=%d frame=%s\n",
                      (unsigned long)mon_.steps(), out.settled ? 1 : 0,
                      out.in_grace ? 1 : 0, out.sticky ? 1 : 0,
                      libName(fr));
        Serial.printf("  x=");
        for (int f = 0; f < prof.n_features; ++f)
          Serial.printf("%s%.4g", f ? "," : "", (double)last_raw_[f]);
        Serial.printf("\n  rel=");
        for (int f = 0; f < prof.n_features; ++f)
          Serial.printf("%s%.3f", f ? "," : "", (double)rel[f]);
        if (fr >= 0) {
          Serial.printf("\n  resid=");
          for (int f = 0; f < prof.n_features; ++f)
            Serial.printf("%s%.3f", f ? "," : "", (double)resid[f]);
        }
        Serial.printf("\n");
        Serial.printf("  match=%s d=%.2f (alarm edge %.1f) conf=%.2f"
                      " | surpr=%.3f score=%.3f base=%.3f\n",
                      libName(out.matched), (double)out.d_match,
                      (double)cfg::QUIET_Q, (double)mon_.confidence(),
                      (double)out.surprise, (double)mon_.score(),
                      (double)mon_.noveltyBase());
        if (out.matched >= 0 && out.matched < lib_.size()) {
          const Condition& mc = lib_.at(out.matched);
          Serial.printf("  gate: theta=%.3f active=%.3f"
                        " band(mu=%.3f sd=%.3f n=%lu/16)"
                        " | rag=%.2f gate_r=%.2f band(mu=%.2f sd=%.2f n=%lu/16)\n",
                        (double)mon_.theta(), (double)out.gate,
                        (double)mc.s_mu,
                        (double)sqrtf(mc.s_m2 / (float)(mc.s_n ? mc.s_n : 1)),
                        (unsigned long)mc.s_n,
                        (double)out.ragg, (double)out.gate_r,
                        (double)mc.r_mu,
                        (double)sqrtf(mc.r_m2 / (float)(mc.r_n ? mc.r_n : 1)),
                        (unsigned long)mc.r_n);
        } else {
          Serial.printf("  gate: theta=%.3f active=%.3f (unknown state)"
                        " | rag=%.2f gate_r=%.2f\n",
                        (double)mon_.theta(), (double)out.gate,
                        (double)out.ragg, (double)out.gate_r);
        }
#if defined(MONITOR_REL_OBS) || defined(MONITOR_COND_OBS)
        Serial.printf("  ev=%+.3f acc=%.2f [alarm@%.0f clear@%.0f cap@%.0f]"
                      " | train=%d lr=%.4g (%.2fx)\n",
                      (double)out.ev, (double)out.acc,
                      (double)cfg::ACC_ALARM, (double)cfg::ACC_CLEAR,
                      (double)cfg::ACC_CAP,
                      out.trained ? 1 : 0,
                      out.lr_frac >= 0
                          ? (double)(cfg::MONITOR_LR * out.lr_frac) : 0.0,
                      (double)out.lr_frac);
#else
        Serial.printf("  latch hi=%d lo=%d | train=%d lr=%.4g (%.2fx)\n",
                      mon_.zHi(), mon_.zLo(),
                      out.trained ? 1 : 0,
                      out.lr_frac >= 0
                          ? (double)(cfg::MONITOR_LR * out.lr_frac) : 0.0,
                      (double)out.lr_frac);
#endif
        if (ep_.isOpen())
          Serial.printf("  ep: open cond=%s n=%lu%s\n", ep_.cond(),
                        (unsigned long)ep_.count(),
                        ep_pend_ >= 0 ? " (batch stored)" : "");
      }

      // NOTE: no supersede on transitions here. A noise flicker looks
      // exactly like a small transition to a vibration node (its fault
      // shifts are within its own noise envelope), so killing open
      // episodes on unsettled steps destroyed batches routinely. An open
      // episode instead stays until it clears quietly, or until a NEW
      // alarm overwrites it (ep_.open resets the sums); its stored batch
      // keeps accumulating settled samples either way.

      if (mon_.thetaReady() && !baseline_registered_) {
        baseline_registered_ = true;
        if (lib_.size() == 0) {
          // first commissioning: the machine is at STOPPED (user procedure
          // 2026-11-15), so the CAL baseline IS the stopped state. Its
          // condition position IS the origin (rel of the CAL mean = 0 on
          // every feature); units stay 0 -> CAL fallback, because a
          // baseline registered without a batch has no measured spread.
          float zero[cfg::MAX_FEATURES] = {};
          lib_.label("op_stopped", zero, zero, "baseline", COND_NORMAL);
          lib_.save(cfg::COND_FILE);
          Serial.println("[LIB] baseline 'op_stopped' registered (normal)");
          Serial.println("[WHY] first commissioning: the CAL baseline"
                         " auto-registers as the origin condition (centroid"
                         " 0, CAL units) - every other machine state will"
                         " alarm and ask until you label it");
        }
      }

      if (out.alarm_open) {
        if (ep_.isOpen())
          ep_.close("superseded", mon_.space(), now);   // new alarm: the
        // old episode's sums are replaced; its STORED batch (if any)
        // stays for late labeling - only ep_pend_ resets
        ep_pend_ = -1;
        ep_ask_dismissed_ = false;      // a fresh episode may ask again
        ep_.open(last_raw_, mon_.space(), now);
        // NO immediate ask (2026-11-19): the episode feeds settled steps
        // below; the ask fires once its batch reaches ASK_MIN_BATCH. An
        // episode opened mid-move also self-resolves: open() names it on
        // the first settled re-identification instead of asking.
        askIfMature(now);
        publishCondition(true);
        saveAll();
      } else if (out.alarm_clear) {
        ep_.close("cleared", mon_.space(), now);
        if (ep_pend_ >= 0) {          // freeze the batch at the settled mean
          pend_.save(cfg::PENDING_FILE);
          ep_pend_ = -1;
        }
        publishCondition(true);
        saveAll();
      } else if (mon_.alarmed() && ep_.isOpen()) {
        // settled-only feeding (2026-11-15 bench finding): a batch that
        // keeps feeding across a state change holds a MIXED distribution -
        // huge spread, huge radius, and the poisoned centroid then
        // captures states it was never labelled for. Feeding settled
        // steps only keeps the batch = ONE state; the identity check
        // below then sees a frozen reference and actually fires.
        if (mon_.settled()) {
          ep_.feed(last_raw_, mon_.space());
          if (ep_pend_ >= 0) {        // the batch tracks the settled mean
            float rm[cfg::MAX_FEATURES], rs[cfg::MAX_FEATURES];
            ep_.relMeans(rm);
            ep_.relStd(rs);
            pend_.update(ep_pend_, rm, rs, ep_.count(), now, mon_.steps());
          }
          // settled identity check (2026-11-15 bench finding): an episode
          // that stays open across a state change otherwise feeds a MIXED
          // batch forever - no new ask, stale cond (the speed_1 episode
          // swallowing speed_2 was the tour's missing asks). Only SETTLED
          // steps are judged: mid-transition the state is meaningless.
          // A drift needs IDENT_DRIFT_N consecutive confirmations: single
          // noise vetoes must not churn episodes.
          bool stale = false;
          int m = lib_.match(last_raw_, mon_.space(), cfg::QUIET_Q);
          if (strcmp(ep_.cond(), "unknown") == 0) {
            if (m >= 0) {
              stale = true;           // the state returned to a KNOWN one
            } else if (ep_pend_ >= 0) {
              stale = batchMoved(last_raw_);
            }
          } else if (m < 0 ||
                     strcmp(lib_.at(m).id, ep_.cond()) != 0) {
            stale = true;             // named episode, state moved away
          }
          if (stale) ++ident_drift_; else ident_drift_ = 0;
          if (ident_drift_ >= cfg::IDENT_DRIFT_N) {
            ident_drift_ = 0;
            Serial.printf("%s[EP] state moved under the episode - "
                          "re-identifying\n", dev_id_);
            ep_.close("superseded", mon_.space(), now);
            ep_pend_ = -1;            // the old batch stays for late labels
            ep_ask_dismissed_ = false;  // a new occurrence may ask again
            ep_.open(last_raw_, mon_.space(), now);
            publishCondition(true);
            saveAll();
          } else if (ep_pend_ < 0 && ident_drift_ == 0) {
            // batch matured AND the current settled step matches no known
            // condition -> store + ask (2026-11-19). ident_drift_ == 0
            // means the state is not mid-re-identification: a state that
            // is resolving into a known condition waits for that instead.
            askIfMature(now);
          }
        }
      } else if (mon_.thetaReady() && !mon_.alarmed() && !out.alarm_clear &&
                 lib_.size() > 0) {
        // Settles 2012 ch. 2.5 myopia tripwire (RESEARCH.md design decisions):
        // a quiet step whose profile matches no centroid within the tripwire
        // radius (MATCH_Q per-feature units, condition space) may be a
        // confidently-blind region. Counted and logged locally only: never
        // published, never asked about. Volatile by design (tripwire reads
        // recent history).
        if (lib_.match(last_raw_, mon_.space()) < 0) {
          ++quiet_unmatched_;
          if (quiet_unmatched_ % cfg::QUM_REPORT_EVERY == 0)
            Serial.printf("[LIB] quiet-unmatched=%lu steps (no centroid within "
                          "q=%.1f units) - possible blind region\n",
                          (unsigned long)quiet_unmatched_, cfg::MATCH_Q);
        }
      }
    }
  }

  if (msg_.connected() && fresh && have_raw_) {
    // State/condition: retained, publish only when state/condition changes
    char key[48];
    snprintf(key, sizeof(key), "%s|%s", mon_.stateString(), conditionId());
    if (strcmp(key, last_cond_key_) != 0) {
      if (msg_.publishCondition(mon_.stateString(), conditionId(),
                                mon_.confidence(), ep_.duration(now), true))
        snprintf(last_cond_key_, sizeof(last_cond_key_), "%s", key);
    }
  }

  if ((mon_.learning() || mon_.thetaReady()) &&
      now - last_model_save_ > cfg::AUTOSAVE_MS)
    saveAll();
}
