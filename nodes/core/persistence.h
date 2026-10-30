// =====================================================================
// persistence.h -- LittleFS persistence for the GENERAL core.
//
//   /model.bin         POD snapshot: score stats, theta, flags, frozen
//                      feature ranges, frozen normalizer moments (v4)
//   /net.bin           TinyRL network weights (Linear layers, verbatim
//                      shape+data walk over nn::Sequential)
//   /conditions.jsonl  condition library (JSONL, human-inspectable)
//
// Flash wear budget: saves happen on episode/label/feedback events plus
// a 5-minute autosave while learning/monitoring (<= 12 writes/hour,
// same policy as the MVP). Weight blob is ~6 KB for the default net.
// =====================================================================
#pragma once
#include <stdint.h>
#include "node_config.h"
#include "tinyrl_monitor.h"

namespace persist {

bool beginFS();                                    // mount (+format on fail)

bool modelSave(const TinyRLConditionMonitor& m);
bool modelLoad(TinyRLConditionMonitor& m);         // false = fresh node
bool netSave(const TinyRLConditionMonitor& m);
bool netLoad(TinyRLConditionMonitor& m);           // false = corrupt/absent

void factoryReset();                               // erase all three files

} // namespace persist
