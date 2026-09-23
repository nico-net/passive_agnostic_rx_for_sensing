#!/usr/bin/env bash
# CPU-only native polar/DCI regression. Never launches nr-uesoftmodem or UHD.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
build=${1:-"$root/cmake_targets/ran_build/build"}
out=${2:-"$(mktemp -d /tmp/agnostic-dci.XXXXXX)"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
printf 'Evidence directory: %s\n' "$out"
nice -n 10 cmake --build "$build" --target test_nr_pdcch_blind_monitor -j2 >"$out/build.log" 2>&1
set +e
timeout 90s nice -n 10 "$build/test_nr_pdcch_blind_monitor" \
  '--gtest_filter=BlindPdcchTest.AutoDci10*' "--gtest_output=xml:$out/focused.xml" >"$out/focused.log" 2>&1
focused=$?
timeout 90s nice -n 10 "$build/test_nr_pdcch_blind_monitor" \
  --gtest_brief=1 "--gtest_output=xml:$out/regression.xml" >"$out/regression.log" 2>&1
regression=$?
set -e
printf 'focused_exit=%s\nregression_exit=%s\n' "$focused" "$regression" | tee "$out/status.txt"
tail -8 "$out/focused.log"
tail -8 "$out/regression.log"
if (( focused != 0 || regression != 0 )); then exit 1; fi
