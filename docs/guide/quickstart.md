# Quick start

This page takes you from clone to a running node: the host suite verifies the whole core without hardware, then one env flashes the real thing.

Requirements: g++ and bash for the host suite; Python plus PlatformIO (`pipx install platformio`) for firmware; an MQTT broker on your LAN. Reference MCU is the YD ESP32-S3 N16R8.

## Clone and verify without hardware

```bash
git clone --recurse-submodules https://github.com/leekaize/TinyRL-Industrial.git
# or, after a plain clone:
git submodule update --init --recursive

bash tests/run_tests.sh        # the host suite; needs g++ and bash only
```

A green run ends with `HOST-TEST GREEN: 6/6 stages passed`. What each stage proves is on the [tests page](/reference/tests); the suite is the gate before any push that touches `nodes/core/`.

## Build, flash, watch

Set identity in `nodes/<specific_node>/main.cpp` (WiFi, broker IP, `dev_id`, UNS base path): that file is the only node-specific file you edit.

```bash
pio run -e <node_specific_env> -t upload
pio device monitor                          # 115200, USB-CDC
```

## Build environments

One env per node-config in `platformio.ini` (lines 46-182). The `_cond` envs are the deployed cond-era firmware; the plain and `_rel` envs keep the era ablation arms runnable.

| Env | Node | Observation era |
|---|---|---|
| `vfd_g100_node` | Power-Node-01 (VFD + PM1125H meter) | frozen z (legacy baseline) |
| `vfd_g100_node_rel` | Power-Node-01 | anchored-ratio levels (rel-obs arm) |
| `vfd_g100_node_cond` | Power-Node-01 | per-condition centered residuals (deployed) |
| `vibration_node` | Vibration-Node-01 (ADXL) | frozen z (legacy baseline) |
| `vibration_node_cond` | Vibration-Node-01 | per-condition centered residuals (deployed) |

## First commands on the serial console

Press `s` to start learning on a representative machine state. The full key map is on the [console page](/guide/console); the lifecycle (what CAL, FIT and MON mean) is on the [lifecycle page](/guide/lifecycle).

## Adding a sensor

The 3-file recipe (sensor package, node folder, build env) is on [adding a sensor](/guide/adding-a-sensor).
