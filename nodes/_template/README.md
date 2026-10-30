# <experiment_name>

**Status:** draft / running ✅ / concluded
**Envs:** `<name>` (esp32dev), `<name>_s3`
**TinyRL pinned at:** `<short hash>` (see `git submodule status`)

## Goal
One sentence: what question does this experiment answer?

## Hypothesis
What you expect to observe, and the acceptance criterion (e.g. "alarm within 3
windows of a x2 RMS change, no false alarm in 10 min at rest").

## Setup
- Board:
- Wiring: (pin table or photo)
- Sensor / actuator notes:

## Run
```bash
pio run -e <name> -t upload && pio device monitor
```

Serial commands: ...

## Results
| Date | Board | Commit | Key params | Outcome |
|---|---|---|---|---|
| 2025-xx-xx | esp32dev | abc1234 | theta=0.41 | criterion met ✅ |

## Observations
- ...

## Data
- `data/2025-xx-xx_cal_serial.txt` — calibration capture
- `data/2025-xx-xx_tap_test.csv`

## Conclusions / Next
- ...
