#!/usr/bin/env bash
# MCU performance + footprint check (real toolchain, real targets).
#
#   bash tests/run_mcu.sh            # build both nodes, report flash/RAM
#
# Builds every node environment with PlatformIO and parses the size
# report. This is the MCU-side performance tracking the host suite cannot
# do: flash and static RAM are the real embedded constraints, and the
# numbers come from the real ESP32-S3 toolchain. Skipped (exit 0) when
# platformio is not installed - the host suite stays the no-toolchain
# path.
set -uo pipefail
cd "$(dirname "$0")/.."

if ! command -v pio >/dev/null 2>&1; then
  echo "=== MCU CHECK SKIPPED: platformio not installed (pip install platformio) ==="
  exit 0
fi

ENVS=("vfd_g100_node" "vibration_node")
echo "== MCU build + footprint: ${ENVS[*]} =="
fail=0
for env in "${ENVS[@]}"; do
  echo "--- $env ---"
  out="$(pio run -e "$env" 2>&1)" || { echo "BUILD FAILED ($env)"; echo "$out" | tail -20; fail=1; continue; }
  echo "$out" | grep -E "^(RAM|Flash):" | sed 's/^/  /'
done

if [ "$fail" -ne 0 ]; then
  echo "=== MCU CHECK FAILED ==="
  exit 1
fi
echo "=== MCU CHECK GREEN: both node images build; sizes above vs the ==="
echo "=== ESP32-S3 budget (app partition ~3 MB flash, 512 KB SRAM,     ==="
echo "=== 8 MB PSRAM for network/optimizer state)                      ==="
