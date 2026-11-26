# Adding a sensor

A new sensor joins the fleet with three files and no changes to the core, and every node inherits core fixes on its next build.

## 1. The sensor package

Create `nodes/sensors/<new>/` and implement the `ISensorNode` contract (`nodes/core/sensor_interface.h`):

`begin / poll / pollPeriodMs / fresh / features / profile / logLine`

`features()` defines the node's perception. Its order and units become the node's identity: they index the TinyRL model and the condition centroids. Get this right first; changing it later invalidates every stored condition and model on the node's flash.

## 2. The node folder

Copy `nodes/vfd_g100_node/main.cpp` to `nodes/<new>_node/main.cpp` and change identity only (WiFi, broker IP, `dev_id`, UNS base path). The whole file is about 20 lines:

```cpp
static G100Sensor  sensor;
static SensorNodeApp app;
void setup() { ...; app.begin(sensor, id); }   // id = dev_id, UNS_BASE, WiFi, broker
void loop()  { app.tick(); delay(2); }
```

(The file is that small by design; the [module map](/system/architecture#module-map) shows what it wires together.)

## 3. The build env

Copy the template env in `platformio.ini` (the `[env]` block plus any node env; `src_dir = nodes` at line 21), change `build_src_filter` to include your sensor package and node folder:

```ini
build_src_filter = +<core/**> +<sensors/<new>/**> +<<new>_node/**>
```

## After the build

Each node keeps its own `/model.bin`, `/net.bin`, `/conditions.jsonl` and `/pending.jsonl` on its flash; nothing is shared between nodes except the label vocabulary, which is fleet-wide by design (labels name conditions, centroids stay per node; RESEARCH.md design decisions, 2026-11-19).

For the contract side, add the node's UNS path and per-topic fields to `uns/uns.json` following the existing Power-Node-01 / Vibration-Node-01 entries. The file is the Tier0-Edge platform's own import/export format; see [the MQTT contract page](/system/contract) for the editing rules.
