# TinyRL-Industrial: Edge Nodes TinyRL Semantic-Context Link on Agentic Workflow

**Repo Keywords:** Industry 5.0, Frugal AI, Edge Node, TinyML, TinyRL, Streaming RL, Predictive Maintenance, Covariate Shift, Semantic Data, Industrial Context, Active Learning, Semantic Communication, UNS, MQTT, PlatformIO

A general edge node core setup (industrial context) wired to the TinyRL library, with a standardised UNS schema and an AI-agent `skills.md`.

## Documentation map

| Doc | Contents |
|---|---|
| [README.md](README.md) | Setup, build, flash, run |
| [RESEARCH.md](RESEARCH.md) | Research questions, experiment log, design rationale |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Block diagrams, memory/compute layouts, ESP32-S3 utilization |
| [literature/](literature/README.md) | Per-paper snapshots + reading index |

## Quick start

```bash
git clone --recurse-submodules https://github.com/leekaize/TinyRL-Industrial.git
# or, after a plain clone:
git submodule update --init --recursive

bash tests/host-test.sh        # verification; needs g++ and bash only
```

- MCU: YD ESP32-S3 N16R8 (reference)
- Python + PlatformIO (`pipx install platformio`), MQTT broker on your LAN

### Adding a new sensor

1. `nodes/sensors/<new>/` — implement `ISensorNode` (`begin / poll / pollPeriodMs / fresh / features / profile / logLine`). `features()` order + units are the node identity — they index the model and condition centroids.
2. `nodes/<new>_node/main.cpp` — copy `vfd_g100_node/main.cpp`, change identity only.
3. `platformio.ini` — copy the template env, set `build_src_filter`.

`nodes/core/**` is never edited per node — fixes land once, every node inherits them.

### Build, flash, watch

Set identity in `nodes/<specific_node>/main.cpp` (WiFi, broker IP, `dev_id`, UNS base path) — the only node-specific file.

```bash
pio run -e <node_specific_env> -t upload
pio device monitor                          # 115200, USB-CDC
```

### First-run lifecycle

1. **IDLE** — press `s` or publish `{"action":"start"}` to `…/Action/feedback`.
2. **LEARN ≈ 5 min CAL** — machine at NORMAL load; ranges freeze.
3. **LEARN ≈ 2 min theta fit** — the net trains on the live stream.
4. **OK** — monitoring. Anomaly ⇒ ALARM ⇒ episode opens ⇒
   - known condition ⇒ labelled immediately
   - `unknown` ⇒ the node publishes an **ask** (§ AI agent)
5. Reboot: `model.bin` + `net.bin` + `conditions.jsonl` restore; monitoring resumes. Reboot mid-learning: CAL restarts.

### Serial console

| Key | Action |
|---|---|
| `s` | start learning |
| `r` | poll sensor now |
| `l` | print condition library |
| `f <label> [normal\|fault]` | label current/open episode |
| `m` | status (step/score/theta/params/heap) |
| `L` | factory reset stored state |

### Wiring

```
   you / MES / ERP ── UNS ── AI agent (agent/skills.md)
                       │  MQTT broker
                       │  semantic only: conditions, episodes, labels
            ┌──────────┴───────────┐
            │  ESP32-S3 node       │
            │  core/ (general)     │ ← TinyRL: Sequential + StreamingOptimizer
            │  sensors/vfd_g100/   │ ← Modbus-RTU to the VFD
            └──────────────────────┘
```

Node → broker (4 topics, schema `uns-v1.1`, details in `uns/uns_schema_v1.json`):
- `…/State/condition` retained — `state` (idle/learn/ok/alarm), `condition_id`, `confidence`
- `…/State/event` — episode open/close/label + summary + `meas:{feature:value}`
- `…/State/node_info` retained — discovery doc (type, features+units, capacity)
- `…/Action/feedback` ← you/agent: `label` / `normal` / `fault` / `start`

An `open` event with `condition_id:"unknown"` is the node asking a question — the whole feedback loop.

## AI agent (agent/skills.md)

Paste-ready skill sheet for any LLM (local or cloud). Six skills:

1. **Intake & registry** — register nodes from `State/node_info`
2. **Answer open episodes** — reply to unknowns with labels
3. **Threshold governance** — `normal`/`fault` feedback tunes theta (±%)
4. **Knowledge-graph orchestration** — link nodes on related machines
5. **Bandwidth discipline** — semantic only, never raw streams
6. **UNS/MES bridging** — map conditions to MES states/ERP events

Point the agent runtime at the broker with this sheet + `uns/`; it runs every node without per-node babysitting.

## UNS integration

- `uns/uns_namespace.json` — site topology + topic map; import into your UNS broker.
- `uns/uns_schema_v1.json` — payload contract v1.1 (breaking changes vs MVP v3 listed inside).
- Plain MQTT JSON — no vendor lock, no special broker.

## Architecture cheat sheet

- **Learning**: one-step prediction net (4→32→32→4, layer-aware sparse init + LayerNorm + LeakyReLU), reward = −surprise, **StreamingOptimizer** update with eligibility traces (`nodes/core/streaming_optimizer.h`; every parameter moves < α per step; v2 ObGD via `-DMONITOR_USE_OBGD`). Score = z-space prediction-error EMA; alarm at theta = μ + 4σ.
- **Two feature spaces**: frozen log-range (condition centroids) and TinyRL z-scores (frozen after theta fit = exact post-boot resume).
- **Persistence (LittleFS)**: `/model.bin` v4 + `/net.bin` (Linear layers verbatim) + `/conditions.jsonl`.
- **Cost**: 1,348 params ≈ 16 KB optimizer state (traces + leaky max + grads); fwd+bwd+update @1 Hz ≪ 0.1% CPU @240 MHz. PSRAM free for bigger nets (`CONFIG_SPIRAM_USE_MALLOC`).
- **Why a streaming optimizer, not the MVP histograms**: histograms saw only consecutive bin pairs; the net learns joint one-step dynamics; the per-parameter update bound keeps anomaly bursts sane.
- **Ported 2026-11-03**: StreamingOptimizer (default) + layer-aware sparse init with connectivity floor — the old uniform 90% rule zeroed ALL first-layer weights at fan-in 4. Rationale in `RESEARCH.md` design decisions.

## Algorithms

**What runs on the node:** Stream-X applied to one-step prediction — Stream-TD mechanics, paper Alg. 7. Our task has no actions: the node only observes and detects. Suitability:

| Algorithm | Needs | Verdict |
|---|---|---|
| **Stream-TD** | observations only | **ours** |
| Stream-Q, Stream-SARSA | discrete actions to take | unwired (no actuation) |
| Stream-AC, Stream-RAC | continuous actions | unwired (no actuation) |

Control variants ship in the TinyRL submodule — wire Stream-Q (discrete) or Stream-AC (continuous) only if nodes later actuate (e.g., VFD setpoints, maintenance triggers).

**Naming:** this is *not* Q-learning. The net's 4 outputs are 4 predicted features, not action-values; there is no action selection anywhere in `nodes/core/`. Update mechanics (traces + optimizer) are shared across the Stream-X family — our code reuses Stream-Q's update pattern minus the action (`tinyrl_monitor.cpp`).

Recipe (all node-local; the schema never tunes these):

| Rule | What it does | Paper |
|---|---|---|
| Normalize | per-feature running mean μ / spread σ; reward (= −surprise) scaled by its running spread | §3.1 |
| Stabilize | LayerNorm before each activation; sparse init (most weights start at 0, layer-aware budget, 95% total) | §3.2 |
| Bound updates | StreamingOptimizer: each parameter moves < α per sample | §3.3, App. A |

Legend: α = step size (max move per update) · δ = surprise (prediction error) · trace = memory of recently-used weights, fades each step · θ = detection threshold · z-space = standardized feature units · EMA = exponential moving average · centroid = average z-profile of a labelled condition.

### Algorithm 1 — Node monitor  (`tinyrl_monitor.h/.cpp`)

```
loop at poll rate (1 Hz):
 1: x ← sensor.features()                  # e.g. 4 telemetry values
 2: x̄ ← (x − μ)/(√σ + ε)                   # Rule 1: standardize input
 3: ŷ ← net(x̄)                             # predict next features from current
 4: δ ← ‖x − ŷ‖ (z-space)                  # surprise = predicted vs actual
 5: update net: trace + StreamingOptimizer(δ)   # Rules 2+3: one small bounded step
 6: score ← EMA(δ)                         # smoothed surprise
 7: if score > θ: open episode             # → Algorithm 2
 8: publish State/event
```

### Algorithm 2 — Conditions + episodes  (`condition_library.*`, `episode.*`)

```
on episode open:
 1: d ← distance(z-profile, each learned centroid)
 2: label ← nearest if d < r, else "unknown"    # unknown = the node's ask → Algorithm 3
 3: on close: store summary in conditions.jsonl
```

### Algorithm 3 — Agent governance  (`agent/skills.md`)

```
on "unknown" event:
 1: agent replies Action/feedback {label: normal | fault | <condition>}
 2: node stores centroid; θ ← θ·(1 ± governance%)
 3: resume MON — next occurrence is labelled locally
```

Which problem each part solves (reading pointers into the Stream-X paper):

| Problem | Solution | Where |
|---|---|---|
| Per-sample learning diverges (no buffer averages noise) | normalize signals + LayerNorm + bounded updates | §2 stream barrier, §3 recipe |
| Update spikes after quiet periods | leaky-max normalizer (vs RMSProp/Adam) | §7 Leaky maximum normalization, App. A |
| Representation drifts on correlated streams | LayerNorm, sparse init | §7 Continual learning (plasticity) |
| No memory for replay buffers | per-sample learning, ~70 KB state | §7 Resource-constrained learning, §5 |

§4.1 shows re-adaptation after environment switches; accumulating *named* conditions is Algorithm 2 + 3.

## Project Structure

```
├── README.md                         integration manual
├── platformio.ini                    one env per node-config, src_dir points into nodes/
│
├── nodes/core/                       GENERAL CORE — sensor-agnostic
│   ├── sensor_interface.h            ISensorNode contract every sensor implements
│   ├── node_config.h                 compile-time limits, topic layout, defaults
│   ├── feature_space.h               frozen log-range condition space (z + bin spaces)
│   ├── tinyrl_monitor.h/.cpp         TinyRL integration: nn::Sequential + streaming score
│   ├── streaming_optimizer.h         StreamingOptimizer (Stream-X v3 App. A)
│   ├── condition_library.h/.cpp      learned centroids (JSONL)
│   ├── episode.h/.cpp                open/feed/close episode lifecycle + summaries
│   ├── persistence.h/.cpp            model.bin v4 + net.bin (weights) + conditions.jsonl
│   ├── uns_messaging.h/.cpp          WiFi/MQTT, 4 topics, State/* + Action/feedback
│   ├── node_runtime.h/.cpp           composition root — the real setup()/loop()
│   ├── console.h/.cpp                serial console: s r l f L m (+ JSON feedback)
│   ├── json_mini.h                   tiny JSON writer for payloads
│   └── esp_shim.c                    getentropy → esp_random (TinyRL TRNG on ESP32)
│
├── nodes/sensors/                    specify each type of sensor
│
├── dashboard/                        node-red/flowfuse dashboard schema
│
├── nodes/vfd_g100_node_01/           node-specific folder
│   └── main.cpp                      node identity (EDIT HERE)
│
├── uns/                              the "product paper" side
│   ├── uns_namespace.json            site topology + topic map → import into UNS broker
│   └── uns_schema_v1.json            payload contract v1.1 (State/*, Action/*)
│
├── agent/
│   └── skills.md                     6-skill sheet for the central AI agent
│
└── tests/                            no-hardware verification (all green)
    ├── stub_esp32/                   Arduino/WiFi/PubSubClient/LittleFS/esp_random stubs
    ├── host-test.sh                  3-stage: syntax 10 TUs → simulation → full link
    └── host-test/
        ├── test_host.cpp             900-step simulation (cal/fit/alarm/persist/library)
        └── linkcheck.cpp             boots the real node on stubs, drives console
```

## References

- Stream-X (streaming deep RL — TinyRL's algorithm lineage): <https://arxiv.org/abs/2410.14606>
- Repository: <https://github.com/mohmdelsayed/TinyRL> (analyzed at commit `82f0774`)
- Documentation: <https://www.tinyrl.ai/docs/> — ESP32 guide: <https://www.tinyrl.ai/docs/esp32/>
- Board: YD ESP32-S3 WROOM N16R8 — <https://www.espboards.dev/esp32/yd-esp32-s3/>
