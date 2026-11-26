# Repository layout

Everything is verified against the working tree (2026-11-25). `TinyRL/` is the git submodule (`TinyRL/src`: the learning library); `docs/` is this site.

```text
├── README.md                         integration manual
├── ARCHITECTURE.md                   node internals + the TinyRL stack on ESP32-S3
├── RESEARCH.md                       RQs, metric set, design decisions, append-only
│                                     experiment log, literature anchors
├── ENHANCEMENTS.md                   what nodes/core adds over the base learner
├── platformio.ini                    src_dir = nodes; one env per node-config
├── package.json                      docs-site scripts (vitepress)
│
├── nodes/
│   ├── core/                         GENERAL CORE - sensor-agnostic, 11 modules
│   │   ├── sensor_interface.h        ISensorNode contract every sensor implements
│   │   ├── node_config.h             compile-time limits, topic layout, defaults
│   │   ├── feature_space.h           frozen anchored-ratio condition space
│   │   ├── tinyrl_monitor.h/.cpp     the monitor: predict, score, evidence, gates
│   │   ├── streaming_optimizer.h     StreamingOptimizer (Stream-X v3 App. A)
│   │   ├── condition_library.h/.cpp  learned centroids (JSONL)
│   │   ├── episode.h/.cpp            open/feed/close episode lifecycle
│   │   ├── pending_store.h/.cpp      stored unknown batches (P0001...)
│   │   ├── persistence.h/.cpp        model.bin + net.bin + conditions + pending
│   │   ├── uns_messaging.h/.cpp      WiFi/MQTT, 5 topics, State/* + Action/feedback
│   │   ├── node_runtime.h/.cpp       composition root - the real setup()/loop()
│   │   ├── console.h/.cpp            serial console + JSON feedback path
│   │   ├── json_mini.h               tiny JSON writer for payloads
│   │   └── esp_shim.c                getentropy -> esp_random (TinyRL TRNG on ESP32)
│   ├── sensors/
│   │   ├── vfd_g100/                 the Power-Node-01 environment (VFD + PM1125H)
│   │   └── vibration_adxl/           the Vibration-Node-01 environment
│   ├── vfd_g100_node/main.cpp        node identity (EDIT HERE)
│   ├── vibration_node/main.cpp       node identity (EDIT HERE)
│   ├── _template/README.md           template for the next node folder
│   └── README.md                     the core/sensor split, port recipe, lifecycle
│
├── dashboard/
│   └── flows.json                    Node-RED operator dashboard (one sheet, 38 nodes)
│
├── uns/
│   └── uns.json                      THE contract file: Tier0-Edge UNS namespace
│                                     (site topology + per-topic field contract)
│
├── agent/
│   └── skills.md                     the 6-skill sheet for the central AI agent
│
├── tools/                            host-side utilities
│   ├── collect_data.py               MODBUS collect campaign capture
│   ├── extract_rig_fixture.py        turn captures into suite fixtures
│   ├── feature_selection.py          FFT feature study + vib profile lock
│   │                                 (+ --capture, --check, --export)
│   ├── rig_tour.py                   scripted rig tour, labels every ask
│   ├── run_ablation.sh               all ABL_* families in parallel
│   ├── seriallog.py                  serial capture: live view + .log + .csv
│   └── stage6.sh                     suite stage 6 alone
│
├── tests/                            no-hardware verification (see Tests)
│   ├── run_tests.sh                  the 6-stage host suite
│   ├── run_mcu.sh                    PlatformIO footprint check
│   ├── replay.sh                     rerun a captured serial session
│   ├── stub_esp32/                   Arduino/WiFi/PubSubClient/LittleFS stubs
│   ├── host/                         suite + bench C++ sources
│   ├── bench/                        the interactive web bench
│   └── logs/                         suite logs (green logs committed)
│
├── data/                             the recorded campaign
│   ├── serial/collect_20261121_211534.csv   the canonical g100 collect (150 rows/condition)
│   ├── serial/collect_20261121_211534.raw.csv  raw backup capture
│   └── analysis/vib_4feat.csv        the locked vib export (100 rows)
│                                     (the rest of data/ is gitignored)
│
├── literature/                       one snapshot file per paper + README + template
├── assets/                           committed figures (regenerable; commands in
│                                     docs/research/results)
├── docs/                             THIS SITE (vitepress; sources + public/assets)
└── TinyRL/                           git submodule: github.com/mohmdelsayed/TinyRL
```

## Naming: paths vs identifiers

The 2026-11-24 consolidation renamed the UNS paths: `Drive-Station-01` -> `Power-Node-01`, `Vibration-Stand-01` -> `Vibration-Node-01` (topics, firmware `UNS_BASE` defines, dashboard, docs). Kept as code identifiers: `dev_id` (`g100-01` / `vib-01`), `node_type` (`vfd_g100` / `vib_adxl`), platformio env names, file names. Old captures and logs therefore show the old paths; new traffic speaks the new ones.

## Data handling

Tracked in git: the canonical collect CSVs and the vib export (the suite's runtime inputs; see `.gitignore`). Gitignored: `data/serial/*` beyond the canonical captures, `data/analysis/*` beyond the vib export, bench runs, suite logs beyond committed green logs, `.pio/`, `.personal/` (session handoffs, Anki deck).
