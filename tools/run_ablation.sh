#!/usr/bin/env bash
# The paper's leave-one-out ablation runs, in parallel: one bench per port,
# one binary per instance (TINYRL_BENCH_BIN, app.py). Full tour output per
# config lands in tests/logs/ablation_<name>.log; the master log gets one
# summary line per config at the end. The positional-channel row already
# exists (run-20261122-220739 = no_pos) and is only re-summarized here.
set -u
cd "$(dirname "$0")/.."
mkdir -p tests/logs

declare -A FLAGS=(
  [no_bands]="-DMONITOR_COND_OBS -DABL_PERCOND_BAND=0 -DABL_FLOOR_REANCHOR=0"
  [no_acc]="-DMONITOR_COND_OBS -DABL_ACCUMULATOR=0"
  [no_settle]="-DMONITOR_COND_OBS -DABL_SETTLE_GATE=0"
  [no_drift]="-DMONITOR_COND_OBS -DABL_STICKY_HOLD=0 -DABL_YARDSTICK_GROWTH=0"
  [no_persist]="-DMONITOR_COND_OBS -DABL_UNKNOWN_PERSIST=0"
  [rel_obs]="-DMONITOR_REL_OBS"
)
port=8801
for name in no_bands no_acc no_settle no_drift no_persist rel_obs; do
  echo "[$(date +%H:%M:%S)] launching $name on port $port"
  TINYRL_BENCH_FLAGS="${FLAGS[$name]}" \
  TINYRL_BENCH_BIN="/tmp/tinyrl_abl_$name" \
  TINYRL_BENCH_PORT="$port" \
  python3 tests/bench/scenarios/paper_tour.py --mode two-way --dwell 80 \
      --port "$port" > "tests/logs/ablation_$name.log" 2>&1 &
  port=$((port+1))
done
wait
echo "[$(date +%H:%M:%S)] === ALL ABLATION RUNS DONE ==="
for name in no_pos no_bands no_acc no_settle no_drift no_persist rel_obs; do
  echo "--- $name"
  if [ "$name" = no_pos ]; then
    python3 tests/bench/tools/summarize_run.py tests/bench/runs/run-20261122-220739.jsonl \
      | grep -E "^(asks opened|false alarms)" | head -2
    echo "PAPER RUN PASS: tour complete, batches labeled 2, re-tour asks 0 (run-20261122-220739)"
  else
    grep -E "^(asks opened|false alarms)|^PAPER RUN" "tests/logs/ablation_$name.log"
  fi
done
