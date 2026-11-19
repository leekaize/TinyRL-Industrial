#!/usr/bin/env bash
# Host-side verification of the GENERAL core against the REAL TinyRL
# headers (Arduino stubs, no hardware). Run from repo root:
#   bash tests/run_tests.sh
# Five stages, each with a greppable *CHECK OK verdict line:
#   1 compile  2 monitor sim  3 whole-node boot  4 tripwire  5 two-node tour
set -eo pipefail
TINYRL="$(dirname "$0")/../TinyRL"
cd "$(dirname "$0")/.."
P=nodes
STUBS="$(dirname "$0")/stub_esp32"
TESTFILES="$(dirname "$0")/host"
INC="-I $STUBS -I $TINYRL/src -I $TINYRL/examples/stream_x/src -I $P/core"
echo "== 1/5 compile check: all 14 TUs vs the real TinyRL headers =="
for f in core/node_runtime.cpp core/uns_messaging.cpp core/console.cpp \
         core/tinyrl_monitor.cpp core/condition_library.cpp core/episode.cpp \
         core/pending_store.cpp core/persistence.cpp sensors/vfd_g100/g100_modbus.cpp \
         sensors/vfd_g100/g100_sensor.cpp vfd_g100_node/main.cpp \
         sensors/vibration_adxl/adxl345.cpp sensors/vibration_adxl/vibration_sensor.cpp \
         vibration_node/main.cpp; do
  g++ -std=c++17 -O3 -DAG_EMBEDDED -fsyntax-only $INC \
      -I $P/sensors/vfd_g100 -I $P/vfd_g100_node -I $P/sensors/vibration_adxl \
      $P/$f
done
echo "   clean"
echo "== 2/5 monitor sim: commission -> quiet -> persist -> alarm -> label =="
g++ -std=c++17 -O3 -DAG_EMBEDDED $INC $STUBS/stub_impl.cpp \
    $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/persistence.cpp $TESTFILES/sim_test.cpp \
    -o /tmp/tinyrl_test_host
/tmp/tinyrl_test_host | tail -3
echo "== 3/5 whole-node boot: link, restore, every console path =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC -I $P/sensors/vfd_g100 -I $P/vfd_g100_node \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp \
    $P/sensors/vfd_g100/g100_modbus.cpp $P/sensors/vfd_g100/g100_sensor.cpp \
    $P/vfd_g100_node/main.cpp $TESTFILES/console_test.cpp -o /tmp/tinyrl_linkcheck
/tmp/tinyrl_linkcheck | tail -1
echo "== 4/5 tripwire: labeled regime adapts+clears, unknown stays alarmed =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp $TESTFILES/tripwire_test.cpp \
    -o /tmp/tinyrl_tripwire
/tmp/tinyrl_tripwire
echo "== 5/5 two-node tour: asks mature, owner-only detection, churn stays quiet =="
g++ -std=c++17 -O2 -DAG_EMBEDDED $INC \
    $STUBS/stub_impl.cpp $P/core/node_runtime.cpp $P/core/uns_messaging.cpp \
    $P/core/console.cpp $P/core/tinyrl_monitor.cpp $P/core/condition_library.cpp \
    $P/core/episode.cpp $P/core/pending_store.cpp $P/core/persistence.cpp $TESTFILES/tour_test.cpp \
    -o /tmp/tinyrl_tour
/tmp/tinyrl_tour
echo "=== HOST-TEST GREEN: 5/5 stages passed ==="
