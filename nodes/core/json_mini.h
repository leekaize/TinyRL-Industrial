// =====================================================================
// json_mini.h -- dependency-free JSON readers for tiny payloads.
// Handles flat, machine-generated JSON only (what this node emits and
// what Action/feedback messages look like). NOT a general parser.
// =====================================================================
#pragma once
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

inline bool jsonStr(const char* j, const char* key, char* out, size_t outlen) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(j, pat);
  if (!p) return false;
  p += strlen(pat); size_t i = 0;
  while (*p && *p != '"' && i < outlen - 1) out[i++] = *p++;
  out[i] = 0; return true;
}

inline long jsonNum(const char* j, const char* key, long def) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(j, pat);
  if (!p) return def;
  return strtol(p + strlen(pat), nullptr, 10);
}

inline float jsonFloat(const char* j, const char* key, float def) {
  char pat[24]; snprintf(pat, sizeof(pat), "\"%s\":", key);
  const char* p = strstr(j, pat);
  if (!p) return def;
  return strtof(p + strlen(pat), nullptr);
}
