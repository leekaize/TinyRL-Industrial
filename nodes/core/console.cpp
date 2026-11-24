// =====================================================================
// console.cpp -- serial CLI implementation (MVP command set + status).
// =====================================================================
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include "console.h"
#include "node_runtime.h"

void Console::execCmd(char* s) {
  char t1[32] = "", t2[32] = "", t3[16] = "";
  if (s[0] == 0) return;
  // word match must come first: "reset" starts with 'r' = poll now
  if (strcmp(s, "reset") == 0) { app_->factoryReset(); return; }
  if (s[0] == 's') { app_->startLearning(); return; }
  if (s[0] == 'r') { app_->sensorPollNow(); return; }
  if (s[0] == 'l') { app_->listLibrary(); return; }
  if (s[0] == 'p') { app_->listPending(); return; }
  if (s[0] == 'm') { app_->printStatus(); return; }
  if (s[0] == 'v') { app_->toggleVerbose(); return; }
  if (s[0] == 'd') { app_->toggleTrace(); return; }
  if (s[0] == 'R') { app_->toggleRawStream(); return; }
  if (s[0] == 'f' && sscanf(s, "f %31s %31s %15s", t1, t2, t3) >= 1) {
    // Grammar (2026-11-20, batch ids 2026-11-21):
    //   f normal | f fault                  bare feedback (no label)
    //   f <label> [normal|fault]            label the OPEN episode
    //   f <id> <label> [normal|fault]       label a STORED batch by id
    //                                       (id "1" = batch P0001)
    // no kind = fault (conservative, matches node_runtime.cpp). A
    // condition named normal/fault is not possible via console - both
    // are reserved feedback words.
    const char* action = "label";
    if (t2[0] == 0 && (strcmp(t1, "normal") == 0 ||
                       strcmp(t1, "fault")  == 0))
      action = t1;                       // bare feedback: label unused
    // batch-id mode: the first token is all digits, or P+digits. A slug
    // can never look like that (slugify keeps letters/digits, but "f 1"
    // as a label is nonsense while "f P0001 ..." is unambiguous).
    bool id_token = false;
    if (t1[0] >= '0' && t1[0] <= '9') {
      id_token = true;
      for (const char* p = t1; *p; ++p)
        if (*p < '0' || *p > '9') { id_token = false; break; }
    } else if (t1[0] == 'P' && t1[1] >= '0' && t1[1] <= '9') {
      id_token = true;
      for (const char* p = t1 + 1; *p; ++p)
        if (*p < '0' || *p > '9') { id_token = false; break; }
    }
    if (id_token && t2[0]) {
      char bid[12];
      if (t1[0] == 'P') snprintf(bid, sizeof(bid), "%s", t1);
      else snprintf(bid, sizeof(bid), "P%04u", (unsigned)atoi(t1));
      char kindj[24] = "";
      if (strcmp(t3, "normal") == 0 || strcmp(t3, "fault") == 0)
        snprintf(kindj, sizeof(kindj), ",\"kind\":\"%s\"", t3);
      char j[176];
      snprintf(j, sizeof(j),
               "{\"action\":\"label\",\"label\":\"%s\"%s,"
               "\"batch\":\"%s\",\"by\":\"console\"}",
               t2, kindj, bid);
      app_->applyFeedback(j);
      return;
    }
    const char* label = t1;
    const char* act   = t2;
    char kindj[24] = "";
    if (strcmp(action, "label") == 0 &&
        (strcmp(act, "normal") == 0 || strcmp(act, "fault") == 0))
      snprintf(kindj, sizeof(kindj), ",\"kind\":\"%s\"", act);
    char j[160];
    snprintf(j, sizeof(j),
             "{\"label\":\"%s\",\"action\":\"%s\"%s,\"by\":\"console\"}",
             label, action, kindj);
    app_->applyFeedback(j);
    return;
  }
  Serial.println("cmds: s=start learning  r=poll now  l=library  "
                 "p=pending batches  f <label> [normal|fault]=label state  "
                 "f <id> <label> [normal|fault]=label batch by id  "
                 "f normal=false alarm  f fault=confirmed  m=status  "
                 "v=bus trace  d=decision trace  R=raw x,y,z stream (vib)  "
                 "reset=factory reset");
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
