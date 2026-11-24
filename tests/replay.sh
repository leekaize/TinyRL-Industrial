#!/usr/bin/env bash
# Offline replay: run a captured serial session through the REAL core.
#
#   python3 tools/seriallog.py                 # on the bench: capture
#   bash tests/replay.sh data/serial/<file>.log  # at the desk: rerun it
#
# The replay re-learns from an empty model at the captured start moment,
# applies the captured labels, and compares every decision (state+cond)
# against the captured ground truth. Change a cfg:: knob, rerun, watch
# the match rate - that is offline tuning without the hardware.
set -eo pipefail
cd "$(dirname "$0")/.."
P=nodes
STUBS=tests/stub_esp32
TINYRL=TinyRL
INC="-I $STUBS -I $TINYRL/src -I $TINYRL/examples/stream_x/src -I $P/core"

g++ -std=c++17 -O2 -DAG_EMBEDDED $INC \
    $STUBS/stub_impl.cpp \
    $P/core/node_runtime.cpp $P/core/uns_messaging.cpp $P/core/console.cpp \
    $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
    tests/host/replay.cpp -o /tmp/tinyrl_replay
/tmp/tinyrl_replay "$1"
