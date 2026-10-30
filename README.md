# TinyRL-Industrial: Edge Nodes TinyRL Semantic-Context Link on Agentic Workflow

Keywords: Industry 5.0, Frugal AI, Edge Node, TinyRL, Streaming RL, Predictive Maintenance, Covariate Shift, Semantic Data, Industrial Context, UNS, MQTT, PlatformIO

The product provides a **general core** wired to the **TinyRL library**, with standardised UNS schema + AI-agent `skills.md`. 

## Documentation map

| Doc | What's in it |
|---|---|
| [README.md](README.md) | (You are here) Guide on how to utilise the library: setup, build, flash, run |
| [RESEARCH.md](RESEARCH.md) | Research questions, progress tracking, experiment log, design rationale |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Deep dive: block diagrams, memory/compute/flash layouts, runtime internals, ESP32-S3 utilization |

## Quick start

```bash
# clones this repo with TinyRL as dependency (submodule)
git clone --recurse-submodules https://github.com/leekaize/TinyRL-Industrial.git

# or, after a plain clone:
git submodule update --init --recursive

# run the verification (needs g++ and bash only)
bash tests/host-test.sh
```

Setup needed:

- A supported MCU (Reference: YD ESP32-S3 N16R8)
- Add sensor following guide below
- Python + PlatformIO (`pipx install platformio`), MQTT broker on your LAN

### Adding a new sensor

1. `nodes/sensors/<new>/` — implement `ISensorNode` (`begin / poll / pollPeriodMs / fresh / features / profile / logLine`). `features()` order + units are the node's identity — they index the model and the condition centroids.
2. `nodes/<new>_node/main.cpp` — copy `vfd_g100_node/main.cpp`, change identity only.
3. `platformio.ini` — copy the commented template env, set `build_src_filter = +<core/**> +<sensors/<new>/**> +<<new>_node/**>`.

`nodes/core/**` is never edited per node — fixes land once, every node inherits them at the next build. For example, a proven Modbus code lives on untouched in `sensors/vfd_g100/`.

### Build, flash, watch

Set your identity in `nodes/<specific_node>/main.cpp` first (WiFi SSID/password, broker IP, `dev_id`, UNS base path) — it is the only file with node-specific identity in it.

```bash
nano platformio.ini                         # set node-specific environment
pio run -e <node_specific_env> -t upload
pio device monitor                          # 115200, USB-CDC
```

### First-run lifecycle (what you will see in the console)

1. **IDLE** — fresh node, restores nothing. Press **`s`** or publish
   `{"action":"start"}` to `…/Action/feedback`.
2. **LEARN ≈ 5 min CAL** — machine must run at NORMAL load; ranges freeze.
3. **LEARN ≈ 2 min theta fit** — the TinyRL net trains on the live stream.
4. **OK** — monitoring. Anomaly ⇒ ALARM ⇒ episode opens ⇒
   - known condition ⇒ labelled immediately,
   - `unknown` ⇒ the node publishes an **ask**; answer it (§ 5).
5. Reboot any time: `model.bin` + `net.bin` + `conditions.jsonl` restore and
   monitoring resumes with no relearning. Reboot mid-learning: CAL restarts
   automatically.

### Serial console

| Key | Action |
|---|---|
| `s` | start learning |
| `r` | poll sensor now |
| `l` | print condition library |
| `f <label> [normal\|fault]` | label current/open episode |
| `m` | status (adds step/score/theta/params/heap) |
| `L` | factory reset stored state |

### Wiring diagram (who talks to whom)

```
   you / MES / ERP ── UNS ── AI agent (agent/skills.md)
                       │  MQTT broker
                       │  semantic only: conditions, episodes, labels
            ┌──────────┴───────────┐
            │  ESP32-S3 node       │
            │  core/ (general)     │ ← TinyRL: Sequential + ObGD
            │  sensors/vfd_g100/   │ ← Modbus-RTU to the VFD
            └──────────────────────┘
```

Node → broker (4 topics, schema `uns-v1.1`, details in `uns/uns_schema_v1.json`):
- `…/State/condition` retained — `state` (idle/learn/ok/alarm), `condition_id`, `confidence`
- `…/State/event` — episode open/close/label + summary + `meas:{feature:value}` (generic)
- `…/State/node_info` retained — discovery doc (type, features+units, capacity)
- `…/Action/feedback` ← you/agent answer: `label` / `normal` / `fault` / `start`

An `open` event with `condition_id:"unknown"` **is the node asking a question**.
That is the whole human/agent feedback loop.

## AI agent (agent/skills.md)

`agent/skills.md` is a ready-to-paste system/skill sheet for any LLM (local
lightweight or cloud). It defines 6 skills:

1. **Intake & registry** — register nodes from `State/node_info`, map node→asset
2. **Answer open episodes** — the core loop: reply to unknowns with labels
3. **Threshold governance** — `normal`/`fault` feedback tunes theta (±%)
4. **Knowledge-graph orchestration** — link nodes that observe related machines
5. **Bandwidth discipline** — never request raw streams; semantic-only by design
6. **UNS/MES bridging** — map conditions to MES states/ERP events

Point your agent runtime at the broker with the skills sheet + the two files in
`uns/`, and it can run every node on site without per-node human babysitting.

## UNS integration

- `uns/uns_namespace.json` — import into your UNS broker/tool: site topology
  and the topic map.
- `uns/uns_schema_v1.json` — the payload contract (v1.1; breaking changes vs
  your MVP v3 are listed inside the file: `meas` map replaces flat
  `i_mean/…` fields, retained `State/node_info` added, `model.bin` bumped to v4).
- Everything is plain MQTT JSON — no vendor lock, no special broker needed.

## Architecture cheat sheet

- **Learning**: one-step prediction net (4→32→32→4, sparse-init Linear +
  LayerNorm + LeakyReLU), reward = −surprise, **ObGD** update with eligibility
  traces + overshooting bound (from `TinyRL/examples/stream_x/src/obgd.h`).
  Score = z-space prediction-error EMA; alarm when it crosses theta = μ + 4σ.
- **Two feature spaces**: frozen log-range (condition library/centroids) and
  TinyRL z-scores (network input; frozen after theta fit = exact post-boot resume).
- **Persistence (LittleFS)**: `/model.bin` v4 (POD stats) + `/net.bin`
  (Linear layers serialized verbatim) + `/conditions.jsonl` (labels, format
  unchanged from MVP — parsing bug fixed).
- **Cost**: 1,348 params ≈ 10 KB with traces+grads; fwd+bwd+ObGD @1 Hz ≪ 0.1% CPU
  @240 MHz. PSRAM stays available for bigger nets via `CONFIG_SPIRAM_USE_MALLOC`.
- **Why ObGD, not the MVP histograms**: histograms only saw consecutive bin
  pairs; the net learns the joint one-step dynamics of all features, and the
  overshooting bound keeps updates sane during anomaly bursts.

## Project Structure

```
├── README.md                         integration manual
├── platformio.ini                    one env per node-config, src_dir points into nodes/
│
├── nodes/core/                       GENERAL CORE — sensor-agnostic
│   ├── sensor_interface.h            ISensorNode contract every sensor implements
│   ├── node_config.h                 compile-time limits, topic layout, defaults
│   ├── feature_space.h               frozen log-range condition space (z + bin spaces)
│   ├── tinyrl_monitor.h/.cpp         TinyRL integration: nn::Sequential + ObGD score
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

- Repository: <https://github.com/mohmdelsayed/TinyRL> (analyzed at commit `82f0774`)
- Documentation: <https://www.tinyrl.ai/docs/> — ESP32 guide: <https://www.tinyrl.ai/docs/esp32/>
- Board: YD ESP32-S3 WROOM N16R8 — <https://www.espboards.dev/esp32/yd-esp32-s3/>
