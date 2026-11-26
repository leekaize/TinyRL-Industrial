# MQTT contract (schema uns-v1.1)

Five topics are the whole integration: import one file, read four, publish to one. The single contract file is `uns/uns.json`: the Tier0-Edge UNS namespace, site topology plus per-topic fields, in the platform's own import/export format. Import it into your UNS platform; the platform exports this same format. Plain MQTT JSON underneath: no vendor lock, no special broker.

## Site topology

Verified against `uns/uns.json` (2026-11-24 consolidation):

```
v1/Plant01/Motor-Test-Area/Line1/
    Power-Node-01/      dev_id g100-01, node_type vfd_g100
    Vibration-Node-01/  dev_id vib-01,   node_type vib_adxl
```

Each node owns five topics: four `State/*` up, one `Action/feedback` down.

## The wire contract

| Topic | Retained | When | Payload |
|---|---|---|---|
| `.../State/condition` | yes | state or condition change, (re)connect | `state` (idle/learn/ok/alarm), `condition_id`, `confidence`, `episode_s`, `batch`, `ts_ms` |
| `.../State/event` | no | every episode transition | `event` (open/close/label), `condition_id`, `severity`, `duration_s`, `summary`, `meas:{feature:value}`, `ts_ms` |
| `.../State/node_info` | yes | every (re)connect | discovery doc: `schema`, `node_type`, `dev_id`, `features` + units, `lib_max`, `fw`, `ts_ms` |
| `.../State/keepalive` | no | every 10 s + (re)connect | `uptime_s`, `rssi`, `free_heap`, `pubs`, `ts_ms`; 30 s of silence = OFFLINE for dashboards |
| `.../Action/feedback` | subscribed, QoS1 | agent or human input | `start`, `label`, `discard`, `normal`, `fault`, `reset` |

Field lists above match the per-topic field definitions in `uns/uns.json`; the publishing code is `nodes/core/uns_messaging.h/.cpp`.

## The ask

An `open` event with `condition_id:"unknown"` is the node asking a question: the whole feedback loop. The sequence:

1. An alarm matches no known condition: the node publishes `unidentified` while the batch matures.
2. At 15 settled samples (`ASK_MIN_BATCH`) the batch is stored (survives reboots, id `P0001...`) and the condition becomes `unknown`. While the ask is live, `State/condition.batch` carries the stored batch id (added 2026-11-24; before that the id existed only on serial).
3. The oracle answers on `Action/feedback`, by batch id.

## Action/feedback semantics

| Action | Payload example | Effect |
|---|---|---|
| `start` | `{"action":"start"}` | begin learning on a representative machine state; the CAL baseline auto-registers as `op_stopped`. With the PM1125H meter in the feature set, commission on a RUNNING state (PF is NaN at no load) |
| `label` | `{"action":"label","label":"belt_slip","by":"agent-01","batch":"P0001"}` | label the STORED batch; late labels work, the episode relabels too. Omit `batch` to label the live episode directly |
| `discard` | `{"action":"discard","batch":"P0001"}` | drop the batch, model untouched; the node will ask again next occurrence |
| `normal` | `{"action":"normal"}` | false alarm: theta +10% |
| `fault` | `{"action":"fault"}` | sharpen: theta -5% |
| `reset` | `{"action":"reset"}` | factory wipe + immediate retained IDLE publish. Human-initiated only (agent/skills.md non-negotiable) |

## Bandwidth discipline

Uplink is semantic only: condition changes and episode summaries, published on change. Never periodic raw telemetry; raw values travel only inside episode summaries (`meas`). Downlink is context only: the only subscribed topic is `Action/feedback`.

Payloads are bounded by the schema buffers: at most 224 B for a state change and 640 B for an episode summary with its per-feature means (`uns_messaging` payload buffers). Measured over the recorded 1,357-step two-node tour: 121 semantic messages vs 2,714 raw vectors at 1 Hz, about 22x fewer (RQ2, [research](/research/questions)).

## Editing uns.json

The file is a machine format, not hand-editable comfortably (single line, no trailing newline). Edits should be scripted (json load/dump with compact separators). The alias fields are platform-generated opaque ids: never invent new ones. After any edit, re-parse the file to prove it still loads. The 2026-11-24 consolidation removed two stale overlapping copies (`uns_namespace.json`, `uns_schema_v1.json`); this file is the one contract.
