# Tests and validation

Three commands reproduce every claim in this project: the host suite, the MCU footprint build, and the web bench. No-hardware verification of the whole node core against the real TinyRL headers (Arduino stubs, g++ and bash only), an interactive web bench for the multi-node scenarios, and an MCU footprint check with the real toolchain. Run from the repo root:

```bash
bash tests/run_tests.sh      # the 6-stage host suite (fast, assert-based)
bash tests/run_mcu.sh        # PlatformIO build: flash/RAM per node vs budget
bash tools/stage6.sh         # stage 6 alone: the cond-era two-node tour
                             # (full output -> tests/logs/stage6_*.log)
```

The suite is the gate after ANY change to `nodes/core/`. The latest green runs are committed as logs: `tests/logs/host_tests_20261124_174158.log` (6/6, after the batch + reset contract changes).

## Real data is the default (2026-11-22c)

The tour stages and the web bench stream the MODBUS collect campaign (`data/serial/collect_20261121_211534.csv`) through `machine_model.hpp`. The vibration node streams its export (`data/analysis/vib_4feat.csv`). Environment switches:

```bash
TINYRL_SYNTH=1      # force the synthetic world everywhere (debugging)
TINYRL_VIB_SYNTH=1  # pin ONLY the vibration node to synthetic
TINYRL_BENCH_FLAGS  # bench era override; default = the deployed cond era.
                    # "-DMONITOR_REL_OBS" = the rel paper arm,
                    # empty = the z era ablation arm
TINYRL_RAMP_SYNTH=0 # ablate the 1-second transition ramps (one midpoint row
                    # per ordered block pair). ON by default: the physical
                    # machine ramps ~1 s between states, so the ramped
                    # fixture is the honest one
```

Detection-chain ablation (the paper's Table A-III): one mechanism family per build, all default ON. Families live in node_config.h: ABL_SETTLE_GATE, ABL_PERCOND_BAND, ABL_FLOOR_REANCHOR, ABL_POS_EVIDENCE, ABL_ACCUMULATOR, ABL_STICKY_HOLD, ABL_YARDSTICK_GROWTH, ABL_UNKNOWN_PERSIST. `-DMONITOR_REL_OBS` ablates the cond-centered observation frame. `tools/run_ablation.sh` runs every family in parallel; summarize with `tests/bench/tools/ablation_table.py`.

## Layout

```text
tests/
├── run_tests.sh              the 6-stage host suite (assert-based, CI-able)
├── run_mcu.sh                real-toolchain build: flash/RAM vs budget
├── replay.sh                 rerun a captured serial session in the host core
├── stub_esp32/               Arduino/WiFi/PubSubClient/LittleFS stubs
├── host/                     C++ sources
│   ├── sim_test.cpp          stage 2/5: cal/fit/quiet/alarm/persist/library + timing
│   ├── console_test.cpp      stage 3: the real node boots on stubs, console driven
│   ├── tripwire_test.cpp     stage 4: quiet-unmatched tripwire semantics
│   ├── tour_test.cpp         stage 6: the cond-era two-node tour on the collect campaign
│   ├── machine_model.hpp     the simulated motor + both sensor models (shared)
│   ├── bench_host.cpp        multi-node host run the web bench drives
│   └── plasticity.hpp        Dohare-2024 counters (dead units, mean |w|, eff rank)
├── bench/                    the interactive web bench
│   ├── bench_server.py       thin launcher (http://127.0.0.1:8765)
│   ├── app.py                Bench driver, HTTP API, run export
│   ├── static/               browser UI (vanilla js + Chart.js)
│   ├── scenarios/            headless drivers, exit code = verdict
│   │   ├── smoke.py          regression: commission -> ask -> label -> re-match
│   │   ├── probe_pending.py  targeted pending-store probe (2 min)
│   │   └── paper_tour.py     the 7-condition paper tour, two-way | node-only
│   ├── tools/                summarize_run.py, plot_run.py, plot_compact.py,
│   │                         diag_space.py, plot_percond_trace.py,
│   │                         plot_percond_tour.py, plot_bench.py,
│   │                         ablation_table.py
│   └── runs/                 exported runs (gitignored)
└── logs/                     suite + stage logs (green logs committed,
                              referenced by handoffs and this site)
```

## The six stages

| Stage | What it builds | What it proves |
|---|---|---|
| 1 compile | every core TU, `-fsyntax-only` | headers and signatures line up (14 TUs) |
| 2 monitor sim | monitor + library + episode + persistence (`sim_test.cpp`) | commissioning, quiet, persistence round-trip, alarm on anomaly, library match, per-step timing |
| 3 whole-node boot | the whole node incl. MQTT/console on stubs | boot, restore, every console path end to end |
| 4 tripwire | the whole core + `tripwire_test.cpp` | training policy: labeled regime trains and clears, unknown regime alarms and stays alarmed until named |
| 5 cond-obs sim | the deployed era on the same fixture (`-DMONITOR_COND_OBS`) | the cond-era contract: in-radius fault CATCHES via the positional band; the z-era MISSES is the A/B contrast |
| 6 real-data tour | two whole nodes on the collect campaign (`tour_test.cpp`) | ask maturity (15+), owner-only asks, re-tour zero asks, churn quiet, full vocabulary |

## What a green run looks like (real output, `host_tests_20261124_174158.log`)

```text
== 1/6 compile check: all 14 TUs vs the real TinyRL headers ==
   clean
== 2/6 monitor sim: commission -> quiet -> persist -> alarm -> label ==
SIMCHECK OK: cal/fit/quiet/persist/alarm/label/timing asserted
== 3/6 whole-node boot: link, restore, every console path ==
=== LINK + SETUP/LOOP + CONSOLE OK ===
== 4/6 tripwire: labeled regime adapts+clears, unknown stays alarmed ==
TRIPCHECK OK: labeled regime trains+clears, unknown regime alarms and stays alarmed until named
== 5/6 cond-obs sim: the DEPLOYED era, same fixture (-DMONITOR_COND_OBS) ==
SIMCHECK OK: cal/fit/quiet/persist/alarm/label/timing asserted
== 6/6 real-data tour: the cond-era two-node tour on the collect campaign ==
TOURCHECK OK: asks mature at 15+, owner-only asks, re-tour zero asks, full vocabulary
=== HOST-TEST GREEN: 6/6 stages passed ===
```

`HOST-TEST GREEN: N/6 stages passed` is the single greppable verdict line. Per-step compute is reported inside stages 2/5 as a host proxy for the MCU budget: the whole per-second workload (forward + bounded update + score EMA + gates) timed on x86; avg 4-5 us, max 136 us over 2000 steps against a 1 s/step budget (the log's `G:` lines).

## MCU footprint (run_mcu.sh, real ESP32-S3 toolchain)

```text
--- vfd_g100_node ---
  RAM:   16.8% (55,076 / 327,680 bytes)
  Flash: 30.7% (1,027,567 / 3,342,336 bytes)
--- vibration_node ---
  RAM:   18.5% (60,724 / 327,680 bytes)
  Flash: 31.6% (1,057,031 / 3,342,336 bytes)
```

Both node images fit comfortably: ~17-19% of static RAM, ~31% of the app partition. Re-run `run_mcu.sh` after any core change; a jump in these numbers is the first sign of an embedded-budget leak.

## Reading the plasticity log (`[PLAST]`)

```
[PLAST] step=339      dead=0.00/0.00 mean|w|=0.0111 eff_rank=2.98 (win=256)
```

| Field | Meaning | Healthy | Worrying (Dohare 2024) |
|---|---|---|---|
| `dead=a/b` | fraction of dead units, hidden layer 1 / layer 2 (near-zero variance pre-LayerNorm) | near 0 | climbing toward Dohare's ~25% |
| `mean\|w\|` | mean absolute Linear weight magnitude (biases excluded) | roughly stable | monotonic growth over long runs |
| `eff_rank` | effective rank of the hidden representation (entropy of the covariance spectrum, Roy & Vetterli) | near the input dimensionality | collapsing toward 1 |
| `win` | audit window: recent raw samples z-scored by a replica of the frozen normalizer | 256 | - |

Long-run bench:

```bash
TINYRL_LONGRUN=100000 bash tests/run_tests.sh     # default 100000 steps
TINYRL_LONGRUN=48000000 bash tests/run_tests.sh   # the 48M-step question, ~30-40 min
```

The stream gets a regime shift at each quarter (machine life); counters log at fit/quiet/alarm checkpoints. 100k result: dead 0.00, mean abs w 0.0111 -> 0.0253, eff rank 4.51 -> 1.88 (2026-11-20).

## Reading the tripwire log (stage 4)

Stage 4 runs the REAL `SensorNodeApp` with a fake sensor, captures the app's own console output, and asserts on it; the full capture stays in `/tmp/tinyrl_tripwire.log`. What a correct run shows there (the 2026-11-15 training policy: the net trains on settled steps of LABELED conditions only; unknown regimes never train):

```text
[RL] theta=1.41 (mu=1.02) - monitoring
[RL] ALARM score=3.67 theta=1.41            <- step to a fault-like level
[EP] open cond=unknown                      <- the ask (alarm-gated, by design)
[LIB] learned new condition 'belt_slip'     <- console f answers it
[RL] clear  score=0.56 theta=1.41           <- LABELED regime: net trains,
                                               adapts, quiet at that level
[RL] ALARM score=6.61 ...                   <- step away from every centroid
[EP] open cond=unknown                      <- stays alarmed: unknown regime
                                               NEVER trains until named
```

Asserted facts: the labeled regime trains and clears; the unknown regime alarms and STAYS alarmed until named; the quiet-unmatched tripwire stays silent in both cases (an alarmed plateau is not quiet-unmatched).

## The web bench (multi-node)

```bash
python3 tests/bench/bench_server.py        # serves http://127.0.0.1:8765
```

N nodes run in lockstep against ONE simulated machine (default 2: vfd_g100 drive side, vib_adxl vibration); the page shows score/theta, per-feature inputs, phases, episodes, pending batches and condition libraries live.

Operating flow: commission (learn) with the machine STOPPED -> start the motor / inject a fault -> answer the ask (one label resolves every pending ask of the machine; faults named with their operating point) -> re-run the same state: matched locally, no ask. That last count is the payoff metric. A label is shared across the machine's nodes as vocabulary; every centroid comes from the node's own batch.

Modes: `--mode two-way` (default, the full closed loop) and `--mode node-only` (asks never leave the node; the paper's baseline). Scenarios: `smoke.py` (headless regression), `probe_pending.py` (pending-store probe), `paper_tour.py --dwell <steps>` (the 7-condition tour; exports runs to `bench/runs/`).

Runs become paper assets with the bench tools (see [results](/research/results)). UNS and the Node-RED dashboard are the deployment-side view; this bench is the research/experiment view. `--ci` runs a scenario headless with a scripted agent.

## Recipes

```bash
bash tests/run_tests.sh                            # full host suite (fast)
bash tests/run_tests.sh | tail -12                 # compact view
TINYRL_LONGRUN=100000 bash tests/run_tests.sh      # 100k-step plasticity bench
bash tests/run_mcu.sh                              # flash/RAM vs budget (pio)
python3 tests/bench/scenarios/probe_pending.py     # pending-store probe
python3 tests/bench/scenarios/smoke.py             # headless regression
python3 tests/bench/scenarios/paper_tour.py --mode two-way   # paper tour
python3 tests/bench/scenarios/paper_tour.py --mode node-only # baseline
python3 tests/bench/tools/summarize_run.py runs/run-a.jsonl runs/run-b.jsonl
```
