#!/bin/bash
# One-shot: the last churn alarm's context (which node, which path)
L=tests/logs/host_tests_20261122_122425.log
N=$(grep -an "RL. ALARM" "$L" | tail -1 | cut -d: -f1)
echo "alarm line: $N"
sed -n "$((N-16)),$((N+3))p" "$L" | grep -a "RAW\|ALARM\|WHY\|EP"
echo "--- re-anchor events in the churn half of the log ---"
TOT=$(wc -l < "$L")
awk -v s=$((TOT/2)) 'NR>s && /re-anchored|recalibrated|band MATURED/ {print NR": "$0}' "$L" | tail -12
