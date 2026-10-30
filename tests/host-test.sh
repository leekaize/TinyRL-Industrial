#!/usr/bin/env bash
# Host-side verification of the GENERAL core against the REAL TinyRL
# headers (Arduino stubs, no hardware). Run from repo root:
#   bash test/host-test.sh
set -e
TINYRL="$(dirname "$0")/../TinyRL"
cd "$(dirname "$0")/.."
P=nodes
STUBS="$(dirname "$0")/stub_esp32"
TESTFILES="$(dirname "$0")/host-test"
INC="-I $STUBS -I $TINYRL/src -I $TINYRL/examples/stream_x/src -I $P/core"
echo "== 1/3 syntax check: all 10 TUs =="
for f in core/node_runtime.cpp core/uns_messaging.cpp core/console.cpp \
         core/tinyrl_monitor.cpp core/condition_library.cpp core/episode.cpp \
         core/persistence.cpp sensors/vfd_g100/g100_modbus.cpp \
         sensors/vfd_g100/g100_sensor.cpp vfd_g100_node/main.cpp; do
  g++ -std=c++17 -O3 -DAG_EMBEDDED -fsyntax-only $INC \
      -I $P/sensors/vfd_g100 -I $P/vfd_g100_node $P/$f
done
echo "   clean"
echo "== 2/3 simulation: cal/fit/quiet/alarm/persist/library =="
g++ -std=c++17 -O3 -DAG_EMBEDDED $INC $STUBS/stub_impl.cpp \
    $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/persistence.cpp $TESTFILES/test_host.cpp \
    -o /tmp/tinyrl_test_host
/tmp/tinyrl_test_host | tail -3
echo "== 3/3 full-node link + boot + console =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC -I $P/sensors/vfd_g100 -I $P/vfd_g100_node \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/persistence.cpp \
    $P/sensors/vfd_g100/g100_modbus.cpp $P/sensors/vfd_g100/g100_sensor.cpp \
    $P/vfd_g100_node/main.cpp $TESTFILES/linkcheck.cpp -o /tmp/tinyrl_linkcheck
/tmp/tinyrl_linkcheck | tail -1
