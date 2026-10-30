// =====================================================================
// console.cpp -- serial CLI implementation (MVP command set + status).
// =====================================================================
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "console.h"
#include "node_runtime.h"

void Console::execCmd(char* s) {
  char label[32] = "", act[16] = "";
  if (s[0] == 0) return;
  if (s[0] == 's') { app_->startLearning(); return; }
  if (s[0] == 'r') { app_->sensorPollNow(); return; }
  if (s[0] == 'l') { app_->listLibrary(); return; }
  if (s[0] == 'm') { app_->printStatus(); return; }
  if (s[0] == 'L') { app_->factoryReset(); return; }
  if (s[0] == 'f' && sscanf(s, "f %31s %15s", label, act) >= 1) {
    if (act[0] == 0) snprintf(act, sizeof(act), "label");
    char j[160];
    snprintf(j, sizeof(j),
             "{\"label\":\"%s\",\"action\":\"%s\",\"by\":\"console\"}",
             label, act);
    app_->applyFeedback(j);
    return;
  }
  Serial.println("cmds: s=start learning  r=poll now  l=library  "
                 "f <label> [normal|fault]  L=factory reset  m=status");
}

void Console::tick() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (idx_) { line_[idx_] = 0; idx_ = 0; execCmd(line_); }
      continue;
    }
    if (idx_ < sizeof(line_) - 1) line_[idx_++] = c;
  }
}
