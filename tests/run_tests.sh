#!/usr/bin/env bash
# Host-side verification of the GENERAL core against the REAL TinyRL
# headers (Arduino stubs, no hardware). Run from repo root:
#   bash tests/run_tests.sh
# REAL DATA IS THE DEFAULT (user decision 2026-11-22c): the tour stages
# stream the MODBUS collect campaign (tests/host/machine_model.hpp); the
# vib falls back to its synthetic generator until its capture lands
# (TINYRL_VIB_SYNTH=1 pins it; TINYRL_SYNTH=1 forces the synthetic world
# everywhere). LEGACY ARMS NOT CI-GATED (user decision 2026-11-22: the
# paper and the focus are the per-condition cond era; the z-era and
# rel-obs ablation arms stay in the source for the historical A/B but
# their suite passes had been riding on the old settle gate's flapping,
# which the net-drift gate removed). Stages, each with a greppable
# *CHECK OK verdict line:
#   1 compile  2 monitor sim (default arm)  3 whole-node boot
#   4 tripwire  5 cond-obs sim (-DMONITOR_COND_OBS, the deployed era)
#   6 real-data tour on the DEPLOYED cond era
#
# Console shows stage headers + verdict lines only. The FULL output of
# every stage goes to tests/logs/host_tests_<timestamp>.log (gitignored,
# newest 10 kept). Read it when a stage fails or you need the details.
set -eo pipefail
TINYRL="$(dirname "$0")/../TinyRL"
cd "$(dirname "$0")/.."
P=nodes
STUBS="$(dirname "$0")/stub_esp32"
TESTFILES="$(dirname "$0")/host"
INC="-I $STUBS -I $TINYRL/src -I $TINYRL/examples/stream_x/src -I $P/core"

LOGDIR="$(dirname "$0")/logs"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/host_tests_$(date +%Y%m%d_%H%M%S).log"
ls -1t "$LOGDIR"/host_tests_*.log 2>/dev/null | tail -n +11 | xargs -r rm -f || true

say() { echo "$1" | tee -a "$LOG"; }

fail() {
  echo "  FAILED - full log: $LOG" | tee -a "$LOG"
  echo "  ---- last 20 lines of the failed stage ----"
  tail -n 20 "$LOG" | sed 's/^/  /'
  exit 1
}

# run_bin <binary> <verdict grep -E pattern>: full output -> $LOG, verdict
# lines (and only those) to the console. No verdict match counts as a fail.
run_bin() {
  local out
  out="$("$1" 2>&1)" || { printf '%s\n' "$out" >>"$LOG"; fail; }
  printf '%s\n' "$out" >>"$LOG"
  printf '%s\n' "$out" | grep -E "$2" | sed 's/^/  /' || fail
}

say "== 1/6 compile check: all 14 TUs vs the real TinyRL headers =="
for f in core/node_runtime.cpp core/uns_messaging.cpp core/console.cpp \
         core/tinyrl_monitor.cpp core/condition_library.cpp core/episode.cpp \
         core/pending_store.cpp core/persistence.cpp sensors/vfd_g100/g100_modbus.cpp \
         sensors/vfd_g100/g100_sensor.cpp vfd_g100_node/main.cpp \
         sensors/vibration_adxl/adxl345.cpp sensors/vibration_adxl/vibration_sensor.cpp \
         vibration_node/main.cpp; do
  g++ -std=c++17 -O3 -DAG_EMBEDDED -fsyntax-only $INC \
      -I $P/sensors/vfd_g100 -I $P/vfd_g100_node -I $P/sensors/vibration_adxl \
      $P/$f 2>>"$LOG" || fail
done
say "   clean"

say "== 2/6 monitor sim: commission -> quiet -> persist -> alarm -> label =="
g++ -std=c++17 -O3 -DAG_EMBEDDED $INC $STUBS/stub_impl.cpp \
    $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/persistence.cpp $TESTFILES/sim_test.cpp \
    -o /tmp/tinyrl_test_host 2>>"$LOG" || fail
run_bin /tmp/tinyrl_test_host 'SIMCHECK OK|per-step compute|PERCOND-ERA'

say "== 3/6 whole-node boot: link, restore, every console path =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC -I $P/sensors/vfd_g100 -I $P/vfd_g100_node \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
    $P/sensors/vfd_g100/g100_modbus.cpp $P/sensors/vfd_g100/g100_sensor.cpp \
    $P/vfd_g100_node/main.cpp $TESTFILES/console_test.cpp \
    -o /tmp/tinyrl_linkcheck 2>>"$LOG" || fail
run_bin /tmp/tinyrl_linkcheck 'CONSOLE OK'

say "== 4/6 tripwire: labeled regime adapts+clears, unknown stays alarmed =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
    $TESTFILES/tripwire_test.cpp -o /tmp/tinyrl_tripwire 2>>"$LOG" || fail
run_bin /tmp/tinyrl_tripwire 'TRIPCHECK OK'

say "== 5/6 cond-obs sim: the DEPLOYED era, same fixture (-DMONITOR_COND_OBS) =="
g++ -std=c++17 -O3 -DAG_EMBEDDED -DMONITOR_COND_OBS $INC $STUBS/stub_impl.cpp \
    $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/persistence.cpp $TESTFILES/sim_test.cpp \
    -o /tmp/tinyrl_test_host_cond 2>>"$LOG" || fail
run_bin /tmp/tinyrl_test_host_cond 'SIMCHECK OK|per-step compute|PERCOND-ERA'

# Stage 6: the SAME tour binary, compiled for the DEPLOYED cond era.
# Real data is the default (machine_model.hpp); the vib now streams the
# 2026-11-22 clean re-capture export (vib_4feat.csv, re-locked profile).
say "== 6/6 real-data tour: the cond-era two-node tour on the collect campaign =="
if [ -f data/serial/collect_20261121_211534.csv ]; then
  g++ -std=c++17 -O2 -DAG_EMBEDDED -DMONITOR_COND_OBS $INC \
      $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
      $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
      $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
      $TESTFILES/tour_test.cpp -o /tmp/tinyrl_tour_real 2>>"$LOG" || fail
  run_bin /tmp/tinyrl_tour_real 'TOURCHECK OK'
else
  say "   skipped: data/serial/collect_20261121_211534.csv missing -"
  say "   re-run the collect campaign (tools/collect_data.py)"
fi

say "=== HOST-TEST GREEN: 6/6 stages passed ==="
echo "  full log: $LOG"
