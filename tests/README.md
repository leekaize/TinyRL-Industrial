# Tests

No-hardware verification of the whole node core against the real TinyRL
headers (Arduino stubs, g++ and bash only), an interactive web bench for
the multi-node scenarios, and an MCU footprint check with the real
toolchain. Run from the repo root:

```bash
bash tests/run_tests.sh      # 4-stage host suite (fast, assert-based)
bash tests/run_mcu.sh        # PlatformIO build: flash/RAM per node vs budget
```

## Layout

```text
tests/
├── run_tests.sh              the 4-stage host suite (assert-based, CI-able)
├── run_mcu.sh                real-toolchain build: flash/RAM vs budget
├── stub_esp32/               Arduino/WiFi/PubSubClient/LittleFS stubs
├── host/                     C++ sources
│   ├── sim_test.cpp          stage 2: cal/fit/quiet/alarm/persist/library + per-step timing
│   ├── console_test.cpp      stage 3: the real node boots on stubs, console driven
│   ├── tripwire_test.cpp     stage 4: quiet-unmatched tripwire semantics
│   ├── bench_host.cpp        multi-node host run the web bench drives
│   └── plasticity.hpp        Dohare-2024 counters (dead units, mean |w|, eff rank)
└── bench/                    the interactive web bench
    ├── bench_server.py       thin launcher (http://127.0.0.1:8765)
    ├── app.py                Bench driver, HTTP API, run export
    ├── static/               browser UI (vanilla js + Chart.js)
    ├── scenarios/            headless drivers, exit code = verdict
    │   ├── smoke.py          regression: commission → ask → label → re-match
    │   ├── probe_pending.py  targeted pending-store probe (2 min)
    │   └── paper_tour.py     the 7-condition paper tour, two-way | node-only
    ├── tools/                analysis of exported runs
    │   ├── summarize_run.py  paper metrics + cross-run contrast table
    │   ├── plot_run.py       per-node figures (truth strip, score/θ, rel rows)
    │   ├── diag_space.py     condition-space geometry: why states match/veto
    │   └── plot_bench.py     sim logs → RESEARCH.md figures
    └── runs/                 exported runs (gitignored)
```

## What a green run looks like (annotated, real output)

```text
== 1/4 syntax check: all 14 TUs ==
   clean
== 2/4 simulation: cal/fit/quiet/alarm/persist/library ==
[PLAST] step=339      dead=0.00/0.00 mean|w|=0.0111 eff_rank=4.54 (win=256)
G: per-step compute (host proxy): avg 8 us, max 636 us over 2000 steps (MCU budget: 1 s/step at 1 Hz)
ALL HOST TESTS PASSED
== 3/4 full-node link + boot + console ==
=== LINK + SETUP/LOOP + CONSOLE OK ===
== 4/4 quiet-unmatched tripwire (real SensorNodeApp, fake sensor) ==
TRIPCHECK OK: labeled regime trains+clears, unknown regime alarms and stays alarmed until named
=== HOST-TEST GREEN: 4/4 stages passed ===
```

| Line | Meaning |
|---|---|
| `1/4 clean` | all 14 translation units compile against the real TinyRL headers with Arduino stubs |
| `2/4 ... PASSED` | 30-step CAL, 30-step theta fit, 200 quiet steps with no false alarm, save/reload round-trip, injected anomaly that alarms, condition-library label/match. Assert-based |
| `G: per-step compute` | host proxy for the MCU budget: the whole per-second workload (forward + bounded update + score EMA + gates) timed on x86. Catches order-of-magnitude regressions; the 1 Hz budget is 1 s/step |
| `3/4 LINK ... OK` | the real node boots on the stubs and every console path (s, m, l, p, f, reset) executes |
| `4/4 TRIPCHECK OK` | the 2026-11-15 training policy holds: a LABELED regime trains + clears, an unknown regime alarms and stays alarmed until named |
| `HOST-TEST GREEN` | the single greppable verdict line |

## Current performance (validated 2026-11-15/16, synthetic bench)

Headline contrast, two-way vs node-only baseline
(`bash tests/bench/scenarios/paper_tour.py` in both modes, then
`summarize_run.py <runs>` prints this table):

| metric | two-way | node-only |
|---|---|---|
| upstream asks (semantic calls) | 10 | 0 (asks stay on the node) |
| labels consumed | 11 | 0 |
| false alarms (per node) | g100=2-3, vib=2-3 | g100=2, vib=3-4 |
| fault identification (owner) | 4/5 hits, latency 0-10 steps | 4/5 hits, latency 0-11 steps |
| condition vocabulary (final) | g100=5, vib=6 | g100=1, vib=1 |

Reading: detection is the SAME without the semantic loop - the payoff of
two-way context is the vocabulary (every condition recognised on the
re-tour with ZERO new asks) and the small, bounded number of upstream
calls. The one owner miss is `speed_1_imbalance` on the drive side: the
VFD registers shift 1.6-2.6% under imbalance, inside any honest radius -
the vibration node owns that fault. That is the cross-modal result, not
a defect.

## MCU footprint (run_mcu.sh, real ESP32-S3 toolchain)

```text
--- vfd_g100_node ---
  RAM:   [==        ]  16.8% (used 55076 bytes from 327680 bytes)
  Flash: [===       ]  30.7% (used 1027567 bytes from 3342336 bytes)
--- vibration_node ---
  RAM:   [==        ]  18.5% (used 60724 bytes from 327680 bytes)
  Flash: [===       ]  31.6% (used 1057031 bytes from 3342336 bytes)
```

Both node images fit comfortably: ~17-19% of static RAM, ~31% of the app
partition. The network + optimizer state (1,348 params, traces + leaky
max, about 16 KB) lives in the 8 MB PSRAM transparently
(`ARCHITECTURE.md`, Part 2). Re-run `run_mcu.sh` after any core change;
a jump in these numbers is the first sign of an embedded-budget leak.

## The four stages

| Stage | What it builds | What it proves | Typical failure caught |
|---|---|---|---|
| 1 syntax | every core TU, `-fsyntax-only` | headers and signatures line up | compile errors, first |
| 2 simulation | monitor + library + episode + persistence + `sim_test.cpp` | learning, alarm, persistence round-trip, library match, per-step timing | learning or persistence regressions |
| 3 link | the whole node incl. MQTT/console on stubs | boot, restore, console paths end to end | cross-module wiring regressions |
| 4 tripwire | the whole core + `tripwire_test.cpp` | training policy: labeled regime clears, unknown stays alarmed | counting the wrong steps |

## Reading the plasticity log (`[PLAST]`, stage 2 and the long-run bench)

```
[PLAST] step=339      dead=0.00/0.00 mean|w|=0.0111 eff_rank=2.98 (win=256)
```

| Field | Meaning | Healthy | Worrying (Dohare 2024) |
|---|---|---|---|
| `dead=a/b` | fraction of dead units, hidden layer 1 / layer 2: units whose pre-LayerNorm activation has near-zero variance across the window (the LeakyReLU adaptation of Dohare's zero-output ReLU test) | near 0 | climbing toward Dohare's ~25% |
| `mean\|w\|` | mean absolute Linear weight magnitude (biases excluded) | roughly stable | monotonic growth over long runs |
| `eff_rank` | effective rank of the hidden representation (entropy of the covariance spectrum, Roy & Vetterli) | near the input dimensionality | collapsing toward 1 |
| `win` | audit window: recent raw samples z-scored by a replica of the monitor's frozen normalizer | 256 | - |

Long-run bench (Dohare's open question, "does the net stay plastic at 48M steps?"):

```bash
TINYRL_LONGRUN=100000 bash tests/run_tests.sh     # default 100000 steps
TINYRL_LONGRUN=48000000 bash tests/run_tests.sh   # the 48M-step question, ~30-40 min
```

The stream gets a regime shift at each quarter (machine life); counters are
logged at fit/quiet/alarm checkpoints on every run and at a fixed interval in
the long run. First 20k-step signal (2026-11-12): dead 0.00 throughout,
mean abs w 0.0184 -> 0.0338, eff rank 3.0 -> 1.2 across regime shifts.

## Reading the tripwire log (stage 4)

Stage 4 runs the REAL `SensorNodeApp` with a fake sensor, captures the app's
own console output, and asserts on it; the full capture stays in
`/tmp/tinyrl_tripwire.log`. What a correct run shows there (2026-11-15
training policy: the net trains on settled steps of LABELED conditions
only; unknown regimes never train):

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

Asserted facts: the labeled regime trains and clears; the unknown regime
alarms and STAYS alarmed until named; the quiet-unmatched tripwire stays
silent in both cases (an alarmed plateau is not quiet-unmatched).

## Interactive web bench (multi-node)

```bash
python3 tests/bench/bench_server.py        # serves http://127.0.0.1:8765
```

N nodes run in lockstep against ONE simulated machine (default 2:
node 0 vfd_g100 = drive side, node 1 vib_adxl = vibration), and the page
shows score/theta, per-feature inputs, phases, episodes, pending batches
and the condition libraries live.

### Operating the bench (2026-11-15 flow)

1. **commission (learn)** button with the machine STOPPED: CAL baseline
   auto-registers as `op_stopped` (normal); both nodes reach `mon`.
2. Start the motor / change setpoint / inject a fault. Transitions are
   gated: nodes decide only on settled states. A settled unknown stores a
   pending batch (`P0001...`) and opens an ask; the run holds.
3. Answer the ask: one label resolves every pending ask of the machine -
   it lands on the asked node's STORED batch (settled data, works late)
   and every node with an open ask relabels its own episode and consumes
   its own batch. Faults are named WITH their operating point
   (`speed_1_imbalance`). The card's pending list can label/discard any
   batch by id at any time.
4. Re-run the same state: matched locally, no ask - that count is the
   payoff metric.

A label is shared across the machine's nodes as vocabulary; every
centroid comes from the node's own batch - no means or centroids cross
nodes.

### Modes

- `--mode two-way` (default): asks reach the operator/agent, labels come
  back, the node learns. The full closed loop.
- `--mode node-only`: asks never leave the node (auto-dismissed), no
  labels exist - detection latency and false alarms only. This is the
  paper's baseline; compare against two-way.

`bench/scenarios/smoke.py` runs the manual loop headless and exits 0 on
pass (regression test for the whole stack). `bench/scenarios/paper_tour.py
--mode two-way|node-only` plays the 7-condition tour (commission stopped →
speed_1 → speed_2 → faults installed while stopped, SPEED set before
START → re-tour), labels every fresh batch from machine truth once it
holds >= 25 settled samples, discards stale batches, exports the run and
prints the summarizer block. `bench/scenarios/probe_pending.py` is a
2-minute targeted probe of the pending-batch plumbing alone (store,
accumulate, discard).

### Runs → paper assets

For the report/paper: **export run** in the browser (or the scenario
drivers export automatically) - the run lands in `bench/runs/
run-<ts>.jsonl`, then:

```bash
python3 tests/bench/tools/summarize_run.py tests/bench/runs/run-<ts>.jsonl
python3 tests/bench/tools/plot_run.py tests/bench/runs/run-<ts>.jsonl --pdf
python3 tests/bench/tools/diag_space.py tests/bench/runs/run-<ts>.jsonl
```

- `summarize_run.py` prints the per-run markdown block + per-fault CSV,
  and the **cross-run contrast table** when given several runs (the
  two-way vs node-only headline above).
- `plot_run.py` renders per-node figures: truth strip vs node-says strip,
  score/theta with phase bands, and per-feature condition-space rows with
  every learned centroid as a dashed line (PNG + vector PDF).
- `diag_space.py` explains WHY states match or veto: CAL anchors, stored
  centroids + units, live positions per true state, per-pair margins.

Real hardware over USB is a planned second source: the node firmware
will emit the same S-line protocol over serial, and `--source serial`
will swap the data source without touching the UI (pending firmware
work; `--source serial` currently exits with that explanation). UNS and
the Node-RED dashboard stay the deployment-side view; this bench is the
research/experiment view. `--ci` runs the same scenario headless: the
scripted agent answers asks and a summary prints.

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
python3 tests/bench/tools/plot_run.py tests/bench/runs/run-<ts>.jsonl --pdf
```

## The RESEARCH.md figures

Two committed figures in `assets/` come from the host bench; both
regenerate in seconds (matplotlib is the only dependency):

```bash
TINYRL_TRACE=1 /tmp/tinyrl_test_host > /tmp/trace.log
TINYRL_LONGRUN=100000 /tmp/tinyrl_test_host > /tmp/bench.log
python3 tests/bench/tools/plot_bench.py /tmp/trace.log /tmp/bench.log
```

- `detection-trace.png`: score vs theta over the stage-2 simulation; the quiet
  plateau stays below 0.7 theta, the anomaly lifts the score about two orders
  of magnitude, and the alarm lands +4 steps after onset. This is the RQ3
  detection-latency plot.
- `plasticity-bench.png`: the three Dohare correlates across the 100k-step
  long run, regime shifts marked. Read it together with the `[PLAST]` table
  above: it is the same counters over time.

More information about PlatformIO Unit Testing:
- https://docs.platformio.org/latest/advanced/unit-testing/index.html
