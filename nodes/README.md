# TinyRL Modular Sensor Node

For modular self-contained ESP32-S3 nodes that watches a machine, learns its "normal" on-device with the **TinyRL** library, and talks **semantic UNS** (conditions and episodes — not telemetry). Split into a **general core** (compiled unchanged on every sensor node) and **environment packages** (one per sensor type).

```
              ┌────────────────────────── product package ──────────────────────────┐
              │  uns/uns_namespace.json   site topology + topic map (import to UNS) │
              │  uns/uns_schema_v1.json   payload contract (State/* + Action/*)     │
              │  agent/skills.md          what the central AI agent does with it    │
              └──────────────────────────────────────────────────────────────────────┘
   central: AI agent (skills.md) ⇄ MQTT broker ⇄ UNS / MES / ERP
                                        │  semantic only: conditions, episodes, labels
        ┌───────────────────────────────┴─────────────────────────────────┐
        │                    ESP32-S3 sensor node                         │
        │   ┌─────────── GENERAL CORE (experiments/core) ──────────────┐  │
        │   │ node_runtime      composition root: setup()/loop() body  │  │
        │   │ tinyrl_monitor    TinyRL: nn::Sequential + ObGD + score  │  │
        │   │ feature_space     frozen log-range condition space       │  │
        │   │ condition_library learned centroids (JSONL, fixed bug)   │  │
        │   │ episode           open/feed/close lifecycle + summaries  │  │
        │   │ uns_messaging     4 MQTT topics + WiFi/MQTT maintenance  │  │
        │   │ persistence       model.bin v4 + net.bin + conditions    │  │
        │   │ console + json_mini + node_config + sensor_interface     │  │
        │   └──────────────────────▲───────────────────────────────────┘  │
        │                          │ ISensorNode contract                 │
        │   ┌──── ENVIRONMENT (experiments/sensors/vfd_g100) ─────────┐  │
        │   │ g100_regs.h  register map + scaling (bench-proven)      │  │
        │   │ g100_modbus  Modbus-RTU transport (verbatim)            │  │
        │   │ g100_sensor  G100Sensor : ISensorNode → {i,slip,vdc,tq} │  │
        │   └─────────────────────────────────────────────────────────┘  │
        └─────────────────────────────────────────────────────────────────┘
```

## What changed vs the MVP (and why)

| Area | MVP (`vfd_streamq.cpp`) | This rewrite |
|---|---|---|
| Learning | hand-rolled 24-bin transition histograms + NLL | TinyRL `nn::Sequential` (sparse Linear → LayerNorm → LeakyReLU ×2) doing **one-step prediction**; reward = −surprise; optimized by **ObGD** (eligibility traces + overshooting bound) from `examples/stream_x/src/obgd.h` |
| Normalization | log-binning only | two spaces with distinct jobs: frozen log-range (condition library) + TinyRL z-scores (network input, frozen after theta fit) |
| Weights | none to persist | `net.bin`: Linear layers serialized verbatim; node resumes monitoring after reboot with zero relearning (plug-and-play) |
| Score | NLL EMA | z-space prediction error EMA (stationary ~O(1) under normal, spikes on regime change) — theta = μ + 4σ unchanged |
| Episodes/library/feedback/UNS | — | **semantics preserved 1:1** (same topics, same actions, same console), but generic over the feature profile |
| `State/event` | flat `i_mean`... | generic `meas:{<feature>:<value>}` built from the profile; discovery added via retained `State/node_info` |
| Bug fix | `libLoad()` used `strtof` without skipping commas → centroids 2..N loaded as 0 after every reboot | fixed (separator guard), verified by round-trip test |

Why ObGD instead of the histogram: the histogram NLL only saw *pairs* of
consecutive bins; the network learns the *joint* one-step dynamics of all 4
features, and ObGD's overshooting bound keeps the update size sane during
the anomaly burst itself (the moments with the biggest gradients are exactly
the moments you don't want rewriting the model). `StreamQ`/`StreamAC` remain
unused here because a condition monitor has no action space — the update
loop mirrors `stream_q.h` so a future control node (setpoints, derating)
can drop StreamQ in beside the monitor.

Resource cost of the TinyRL step (H=32, nf=4): **1,348 params ≈ 10 KB with
traces + gradients**, forward+backward+ObGD at 1 Hz ≪ 0.1% CPU on the LX7
@240 MHz. `AG_ESP32_DSP` routes `trace_update_l1` / `fma_scalar` through the
hand-tuned ESP32-S3 kernels; `CONFIG_SPIRAM_USE_MALLOC` keeps the
transparent-PSRAM behavior for future bigger nets.

## Repository layout

```
platformio.ini                  [env:vfd_g100_node] + template for the next node
TinyRL/                         git clone github.com/mohmdelsayed/TinyRL (sibling)
experiments/
  core/                         GENERAL — compiled for every node, nobody touches it
  sensors/vfd_g100/             ENVIRONMENT — the only VFD-specific code
  vfd_g100_node/main.cpp        node identity (EDIT THESE) + 20 lines of wiring
uns/  agent/                    schema + AI-agent skill sheet
```

`main.cpp` is the whole node:

```cpp
static G100Sensor  sensor;
static SensorNodeApp app;
void setup() { ...; app.begin(sensor, id); }   // id = dev_id, UNS_BASE, WiFi, broker
void loop()  { app.tick(); delay(2); }
```

## Port to a new sensor (the 3-file recipe)

1. `experiments/sensors/<new>/` — implement `ISensorNode`:
   `begin/poll/pollPeriodMs/fresh/features/profile/logLine`.
   `features()` defines the node's perception — order and units become the
   node's identity (they index the TinyRL model + condition centroids).
2. `experiments/<new>_node/main.cpp` — copy the vfd one, change identity.
3. `platformio.ini` — copy the template env, change `build_src_filter`
   to `+<core/**> +<sensors/<new>/**> +<<new>_node/**>`.

`core/**` is never modified per node — every node inherits core fixes on
the next build. Each node keeps its own `/model.bin`, `/net.bin`,
`/conditions.jsonl` on its flash.

## MQTT contract (schema uns-v1.1)

- `…/State/condition` (retained, on change): `state` idle|learn|ok|alarm,
  `condition_id` idle|learning|normal|<label>|**unknown**, `confidence`.
- `…/State/event` (per transition): open/close/label + `summary` +
  `meas:{feature:value,...}`. An `open` with `condition_id:"unknown"` is
  the node's implicit ASK — answer on `Action/feedback`:
  - `{"action":"label","label":"belt_slip","by":"agent-01"}`
  - `{"action":"normal"}` (false alarm → theta +10%), `{"action":"fault"}` (−5%),
  - `{"action":"start"}` (begin learning; machine at NORMAL load).
- `…/State/node_info` (retained, on connect): discovery doc for the agent.

Console (unchanged): `s` start learning · `r` poll now · `l` library ·
`f <label> [normal|fault]` · `L` factory reset · `m` status (adds step/score/
theta/params/heap).

## Life cycle

boot → restore `model.bin` + `net.bin` + `conditions.jsonl`
→ theta ready ⇒ straight back to monitoring (no relearning)
→ fresh ⇒ IDLE until `s` / `{"action":"start"}` → ~5 min CAL (ranges) →
~2 min theta fit (network trains on the stream the whole time) →
monitoring: alarm ⇒ episode opens ⇒ known ⇒ labelled at once, unknown ⇒
ask ⇒ label ⇒ recognised on-device forever (centroid refined by EMA on
every confirmed close). Reboot mid-learning: CAL restarts automatically
(improvement over MVP's "press 's' again").

## Build & verify

```bash
git clone https://github.com/mohmdelsayed/TinyRL TinyRL
pio run -e vfd_g100_node -t upload && pio device monitor
```

Host-side verification (no hardware) — this rewrite was validated by
compiling every module against the real TinyRL headers with Arduino stubs
and running a full simulation (`scripts/test_host.cpp`, 900 steps):

- CAL freezes ranges → theta fit → `theta=0.35` on the synthetic stream
- 200 quiet steps: **no false alarm**
- injected anomaly: **ALARM after 4 steps** (score 104.7 vs theta 0.35)
- `model.bin`+`net.bin` round-trip: restored node reproduces the score
  (89.06 vs 89.13) and matches theta exactly
- condition label → save → reload → **anomaly matched, normal rejected**
- console `s/m/l/f` paths exercised end-to-end on the linked binary

## Migration notes

- Old `/model.bin` (v3 histogram) is ignored once; press `s` to relearn.
- `State/event` `meas` replaces the flat `i_mean/…` fields (schema v1.1).
- The centroid-reload bug fix changes `conditions.jsonl` **parsing only** —
  the file format is unchanged, existing labels survive the upgrade.
