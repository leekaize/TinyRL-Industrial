# TinyRL-Industrial: Edge Nodes TinyRL Semantic-Context Link on Agentic Workflow

**Repo Keywords:** Industry 5.0, Frugal AI, Edge Node, TinyML, TinyRL, Streaming RL, Predictive Maintenance, Covariate Shift, Semantic Data, Industrial Context, Active Learning, Semantic Communication, UNS, MQTT, PlatformIO

Intelligent sensing nodes monitor a machine and learn its normal states on-device with streaming reinforcement learning. They work with a human or an AI agent: context flows down, conditions flow up, alarms go out only when something needs attention.

## Quick start

```bash
git clone --recurse-submodules https://github.com/leekaize/TinyRL-Industrial.git
# or, after a plain clone:
git submodule update --init --recursive

bash tests/run_tests.sh        # host suite; needs g++ and bash only
```

Build and flash (identity lives in `nodes/<node>/main.cpp`):

```bash
pio run -e <node_env> -t upload
pio device monitor                          # 115200, USB-CDC
```

## Documentation

The full documentation lives in [`docs/`](docs/) and is published at
<https://tinyrl-industrial.leekaize.com>.

| Section | Covers |
|---|---|
| [Guide](docs/guide/) | quick start, first-run lifecycle, serial console, adding a sensor, dashboard, the AI agent |
| [System](docs/system/) | node architecture, the TinyRL stack on ESP32-S3, MQTT contract, algorithms, enhancements |
| [Research](docs/research/) | research questions, design decisions, results and figures, literature |
| [Reference](docs/reference/) | tests and validation, repository layout, references |

Key files:

- `uns/uns.json` - the machine-readable MQTT contract (import into Tier0-Edge)
- `agent/skills.md` - the paste-ready skill sheet for the central AI agent
- `RESEARCH.md` - the research notebook: RQs, design decisions, append-only experiment log
- `dashboard/flows.json` - the Node-RED operator dashboard
