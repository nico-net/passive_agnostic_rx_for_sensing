#!/usr/bin/env bash
# P07 (adaptive_RX_pipeline.md Stage 2, gate G2) -- G2 tests 1 and 3 in REPLAY form.
#
# Replays the P02 baseline fixture through the real nr-uesoftmodem binary (no radio is opened:
# nr-uesoftmodem.c returns from nr_passive_replay_read() before any device init) once per
# single-branch view (ISAC_DL_BRANCH_VIEW=0..3) and once on the legacy 4-antenna path, and
# records per view: crc_ok / crc_fail / unsupported / unsupported_multilayer / data_submits plus
# the per-job PDSCH-VERDICT trace.
#
# PASS criteria (the brief's (i)-(iii), nothing else):
#   (i)   every branch view runs to "no radio opened" and every TB it CRC-accepts is the SAME
#         payload the 4-antenna reference recorded (the binary's own "identical DL controls=N"
#         hash check, N == the view's crc_ok). The binary exits 2 ("REPLAY VOID") whenever a view
#         accepts FEWER TBs than the reference -- that is the expected coverage loss, so exit 2 is
#         accepted for a view; any other non-zero exit, a missing verdict line or N != crc_ok fails;
#   (ii)  the legacy path is UNCHANGED: "REPLAY PASS: identical DL controls=34 failed=0 raw UL=24";
#   (iii) a branch's data-aided submissions == that branch's own CRC-OK count (a branch with
#         0 CRC-OK has 0 submits) -- G2 test 3: no branch obtains data-aided CFR from another
#         branch's payload.
# A single-branch CRC count BELOW the legacy 34 is EXPECTED on this rig (branches 1-3 measured
# 1.7-15 dB down) and is recorded as coverage loss, never tuned away and never a failure here.
#
# Exit codes: 0 PASS, 1 FAIL, 77 SKIP (fixture/binary missing -- ctest SKIP_RETURN_CODE).
# Env: FIXTURE=<dir> (default: the registered P02 fixture), OUT=<dir> for logs, VIEWS="0 1 2 3".
set -uo pipefail

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD="$REPO/cmake_targets/ran_build/build"
FIXTURE="${FIXTURE:-/home/sens/NICOLA/captures/sensing_manual_fixed.UtvBT7}"
VIEWS="${VIEWS:-0 1 2 3}"
EXPECT_LEGACY="${EXPECT_LEGACY:-REPLAY PASS: identical DL controls=34 failed=0 raw UL=24; no radio opened}"

if [ ! -r "$FIXTURE/replay.bin" ] || [ ! -r "$FIXTURE/receiver.conf" ]; then
  echo "SKIP: fixture not readable at $FIXTURE (need replay.bin + receiver.conf) -- not a PASS"
  exit 77
fi
if [ ! -x "$BUILD/nr-uesoftmodem" ]; then
  echo "SKIP: $BUILD/nr-uesoftmodem not built -- not a PASS"
  exit 77
fi
OUT="${OUT:-$(mktemp -d /tmp/replay_branch_view.XXXXXX)}"
mkdir -p "$OUT"
if pgrep -x nr-uesoftmodem >/dev/null; then
  echo "NOTE: another nr-uesoftmodem is running on this host; the replay opens no radio and proceeds"
fi

# The registered fixture's own replay command (tests/passive_rx/baselines/fixtures.json,
# replay_verification.command): tests/passive_rx/run_manual_x410.sh's args minus --usrp-args.
ARGS=(-O "$FIXTURE/receiver.conf" -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150
      --ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation
      --cont-fo-comp 1 --freq-sync-P 0.05 --freq-sync-I 0.001 --initial-fo -16480
      --thread-pool 0,1,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90)

run_one() { # $1 = label, $2 = ISAC_DL_BRANCH_VIEW value or "" (legacy)
  local label="$1" view="$2" log="$OUT/$1.log" rc
  ( cd "$BUILD" && env ISAC_PASSIVE_REPLAY_INPUT="$FIXTURE/replay.bin" ISAC_PDSCH_VERDICT_TRACE=1 \
      ${view:+ISAC_DL_BRANCH_VIEW=$view} LD_LIBRARY_PATH="$BUILD:/usr/local/lib" \
      ./nr-uesoftmodem "${ARGS[@]}" ) > "$log" 2>&1
  rc=$?
  echo "$rc" > "$OUT/$1.exit"
  return $rc
}

field() { # $1 = line, $2 = key -> value
  printf '%s\n' "$1" | grep -oE "(^| )$2=[-0-9]+" | head -1 | cut -d= -f2
}

fail=0
printf '%-8s %5s %6s %6s %8s %6s %5s %11s %13s %12s %s\n' \
  view phys recs crc_ok crc_fail unsup error multilayer data_submits exit verdict | tee "$OUT/summary.txt"

# ---- (ii) legacy 4-antenna reference
run_one legacy ""; rc=$?
legacy_line=$(grep -m1 '^REPLAY ' "$OUT/legacy.log" || true)
view_line=$(grep -m1 '^REPLAY-VIEW ' "$OUT/legacy.log" || true)
if [ "$rc" -eq 0 ] && [ "$legacy_line" = "$EXPECT_LEGACY" ]; then verdict=PASS; else verdict=FAIL; fail=1; fi
printf '%-8s %5s %6s %6s %8s %6s %5s %11s %13s %12s %s\n' legacy "$(field "$view_line" phys)" \
  "$(field "$view_line" records)" "$(field "$view_line" crc_ok)" "$(field "$view_line" crc_fail)" \
  "$(field "$view_line" unsupported)" "$(field "$view_line" error)" \
  "$(field "$view_line" unsupported_multilayer)" "$(field "$view_line" data_submits)" "$rc" \
  "$verdict ($legacy_line)" | tee -a "$OUT/summary.txt"

# ---- (i) + (iii) per branch view
for v in $VIEWS; do
  run_one "view$v" "$v"; rc=$?
  line=$(grep -m1 '^REPLAY-VIEW ' "$OUT/view$v.log" || true)
  phys=$(field "$line" phys); ok=$(field "$line" crc_ok); sub=$(field "$line" data_submits)
  vline=$(grep -m1 -E '^REPLAY (PASS|VOID): identical DL controls=' "$OUT/view$v.log" || true)
  ident=$(printf '%s\n' "$vline" | grep -oE 'controls=[0-9]+' | cut -d= -f2)
  verdict=PASS
  if { [ "$rc" -ne 0 ] && [ "$rc" -ne 2 ]; } || ! printf '%s\n' "$vline" | grep -q 'no radio opened'; then
    verdict="FAIL(i: exit=$rc verdict='$vline')"; fail=1
  elif [ -z "$ident" ] || [ "$ident" != "$ok" ]; then
    verdict="FAIL(i: identical=$ident != crc_ok=$ok)"; fail=1
  fi
  if [ "$phys" != "$v" ]; then verdict="FAIL(view not applied: phys=$phys)"; fail=1; fi
  if [ -z "$ok" ] || [ -z "$sub" ] || [ "$ok" != "$sub" ]; then verdict="FAIL(iii: crc_ok=$ok submits=$sub)"; fail=1; fi
  printf '%-8s %5s %6s %6s %8s %6s %5s %11s %13s %12s %s\n' "view$v" "$phys" \
    "$(field "$line" records)" "$ok" "$(field "$line" crc_fail)" "$(field "$line" unsupported)" \
    "$(field "$line" error)" "$(field "$line" unsupported_multilayer)" "$sub" "$rc" "$verdict ($vline)" \
    | tee -a "$OUT/summary.txt"
done

echo "logs: $OUT (per-job trace: grep '^PDSCH-VERDICT' $OUT/*.log)"
if [ "$fail" -eq 0 ]; then echo "dl_branch_view_replay: PASS"; exit 0; fi
echo "dl_branch_view_replay: FAIL"; exit 1
