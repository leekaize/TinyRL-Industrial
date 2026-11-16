// =====================================================================
// pending_store.h -- flash-backed batches of settled UNKNOWN states.
//
// User workflow (2026-11-15): when a settled state matches no known
// condition, the node stores the batch (condition-space mean + spread +
// sample count) in flash and asks. The operator labels or discards BY
// ID, possibly much later: batches survive reboots and re-appear after
// restart. Labels learn from the STORED batch, never from live
// transition data. Stored in the CONDITION space (rel decades): the
// centroid and matching units a label installs are exactly these.
// =====================================================================
#pragma once
#include <stdint.h>
#include "node_config.h"

struct PendingBatch {
  char     id[12];                     // "P0001"...
  float    rmean[cfg::MAX_FEATURES];   // rel decades, settled mean
  float    rstd[cfg::MAX_FEATURES];    // rel decades, settled spread
  uint32_t n;                          // samples in the batch
  uint32_t first_ms, last_ms;          // device millis
  uint32_t last_step;                  // monitor step of the last update
};

class PendingStore {
public:
  void begin(int nf);

  // store a settled batch; returns index, or -1 impossible (full drops
  // the OLDEST batch so recent states stay answerable)
  int  add(const float* rmean, const float* rstd, uint32_t n, uint32_t now,
           uint32_t step);

  int  find(const char* id) const;
  bool remove(const char* id);         // consumed by label / discard
  // refresh a batch while its episode keeps feeding (the stored mean
  // tracks the SETTLED state, never the onset sample)
  void update(int i, const float* rmean, const float* rstd, uint32_t n,
              uint32_t now, uint32_t step);
  int  size() const { return n_; }
  const PendingBatch& at(int i) const { return p_[i]; }
  void list() const;                   // console dump

  bool save(const char* path) const;
  bool load(const char* path);

private:
  PendingBatch p_[cfg::PENDING_MAX];
  int      n_  = 0;
  int      nf_ = 0;
  uint32_t seq_ = 0;                   // id counter (restored on load)
};
