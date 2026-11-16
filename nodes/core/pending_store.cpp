// =====================================================================
// pending_store.cpp -- flash-backed batches of settled UNKNOWN states.
// JSONL persistence, one batch per line, human-inspectable (same style
// as conditions.jsonl). Values are condition-space (rel decades).
// =====================================================================
#include <Arduino.h>
#include <LittleFS.h>
#include <stdio.h>
#include <string.h>
#include "pending_store.h"
#include "json_mini.h"

void PendingStore::begin(int nf) {
  nf_ = nf;
  n_ = 0;
  seq_ = 0;
}

int PendingStore::add(const float* rmean, const float* rstd, uint32_t n,
                      uint32_t now, uint32_t step) {
  if (n_ >= cfg::PENDING_MAX) {
    // full: drop the OLDEST so recent states stay answerable
    Serial.println("[PEND] store full - oldest batch dropped");
    for (int i = 1; i < n_; ++i) p_[i - 1] = p_[i];
    n_--;
  }
  PendingBatch& b = p_[n_];
  memset(&b, 0, sizeof(b));
  snprintf(b.id, sizeof(b.id), "P%04u", (unsigned)(++seq_));
  for (int f = 0; f < nf_; ++f) {
    b.rmean[f] = rmean[f];
    b.rstd[f] = rstd[f];
  }
  b.n = n;
  b.first_ms = now;
  b.last_ms = now;
  b.last_step = step;
  return n_++;
}

int PendingStore::find(const char* id) const {
  for (int i = 0; i < n_; ++i)
    if (strcmp(p_[i].id, id) == 0) return i;
  return -1;
}

bool PendingStore::remove(const char* id) {
  int i = find(id);
  if (i < 0) return false;
  for (int k = i + 1; k < n_; ++k) p_[k - 1] = p_[k];
  n_--;
  return true;
}

void PendingStore::update(int i, const float* rmean, const float* rstd,
                          uint32_t n, uint32_t now, uint32_t step) {
  if (i < 0 || i >= n_) return;
  for (int f = 0; f < nf_; ++f) {
    p_[i].rmean[f] = rmean[f];
    p_[i].rstd[f] = rstd[f];
  }
  p_[i].n = n;
  p_[i].last_ms = now;
  p_[i].last_step = step;
}

void PendingStore::list() const {
  Serial.printf("[PEND] %d stored batch(es):\n", n_);
  for (int i = 0; i < n_; ++i) {
    const PendingBatch& b = p_[i];
    Serial.printf("  %-8s n=%-5lu first=%lu last=%lu mean=[", b.id,
                  (unsigned long)b.n, (unsigned long)b.first_ms,
                  (unsigned long)b.last_ms);
    for (int f = 0; f < nf_; ++f)
      Serial.printf("%s%.3g", f ? " " : "", b.rmean[f]);
    Serial.println("]");
  }
}

bool PendingStore::save(const char* path) const {
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.println("[FS] pending write FAILED"); return false; }
  f.printf("seq %lu\n", (unsigned long)seq_);
  for (int i = 0; i < n_; ++i) {
    const PendingBatch& b = p_[i];
    f.printf("{\"id\":\"%s\",\"n\":%lu,\"first\":%lu,\"last\":%lu,"
             "\"step\":%lu,\"mean\":[",
             b.id, (unsigned long)b.n, (unsigned long)b.first_ms,
             (unsigned long)b.last_ms, (unsigned long)b.last_step);
    for (int k = 0; k < nf_; ++k)
      f.printf("%s%.5g", k ? "," : "", b.rmean[k]);
    f.printf("],\"sd\":[");
    for (int k = 0; k < nf_; ++k)
      f.printf("%s%.5g", k ? "," : "", b.rstd[k]);
    f.printf("]}\n");
  }
  f.close();
  return true;
}

bool PendingStore::load(const char* path) {
  n_ = 0;
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  while (f.available() && n_ < cfg::PENDING_MAX) {
    String line = f.readStringUntil('\n');
    const char* s = line.c_str();
    if (strncmp(s, "seq ", 4) == 0) {
      seq_ = (uint32_t)atoi(s + 4);
      continue;
    }
    if (strlen(s) < 10) continue;
    PendingBatch& b = p_[n_];
    memset(&b, 0, sizeof(b));
    const char* idp = strstr(s, "\"id\":\"");
    if (!idp) continue;
    idp += 6;
    const char* ide = strchr(idp, '"');
    if (!ide || ide - idp >= (int)sizeof(b.id)) continue;
    memcpy(b.id, idp, ide - idp);
    b.id[ide - idp] = 0;
    // strtof does NOT skip separators: walk with the same guard as the
    // condition library loader
    const char* p = strstr(s, "\"mean\":[");
    if (p) {
      p += 8;
      for (int k = 0; k < nf_; ++k) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p || *p == ']') break;
        b.rmean[k] = strtof(p, (char**)&p);
      }
    }
    p = strstr(s, "\"sd\":[");
    if (p) {
      p += 6;
      for (int k = 0; k < nf_; ++k) {
        while (*p == ',' || *p == ' ') ++p;
        if (!*p || *p == ']') break;
        b.rstd[k] = strtof(p, (char**)&p);
      }
    }
    b.n        = (uint32_t)jsonNum(s, "n", 0);
    b.first_ms = (uint32_t)jsonNum(s, "first", 0);
    b.last_ms  = (uint32_t)jsonNum(s, "last", 0);
    b.last_step = (uint32_t)jsonNum(s, "step", 0);
    if (b.id[0]) n_++;
    unsigned v = (unsigned)atoi(b.id + 1);
    if (v > seq_) seq_ = v;
  }
  f.close();
  return true;
}
