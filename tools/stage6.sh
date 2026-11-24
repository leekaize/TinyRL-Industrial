#!/usr/bin/env bash
# Stage 6 only: the cond-era two-node real-data tour (the suite's churn
# race lives here). Usage: bash tools/stage6.sh [extra args to tour binary]
# Full output -> tests/logs/stage6_<timestamp>.log; verdict lines to stdout.
set -eo pipefail
cd "$(dirname "$0")/.."
P=nodes
STUBS=tests/stub_esp32
TESTFILES=tests/host
INC="-I $STUBS -I TinyRL/src -I TinyRL/examples/stream_x/src -I $P/core \
     -I $P/sensors/vfd_g100 -I $P/vfd_g100_node -I $P/sensors/vibration_adxl"

LOGDIR=tests/logs
mkdir -p "$LOGDIR"
LOG="$LOGDIR/stage6_$(date +%Y%m%d_%H%M%S).log"
ls -1t "$LOGDIR"/stage6_*.log 2>/dev/null | tail -n +11 | xargs -r rm -f || true

g++ -std=c++17 -O2 -DAG_EMBEDDED -DMONITOR_COND_OBS $INC \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
    $TESTFILES/tour_test.cpp -o /tmp/tinyrl_tour_real 2>"$LOG"

/tmp/tinyrl_tour_real "$@" 2>&1 | tee "$LOG" | grep -E \
  'TOURCHECK|ask on|learned|recognised|commissioned|CHURN|churn|OK|FAIL|Assert|assert' \
  || { echo "  FAILED - full log: $LOG"; tail -n 30 "$LOG"; exit 1; }
echo "  full log: $LOG"
