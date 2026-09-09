#!/usr/bin/env bash
# Bounded receiver-only run. No pkill, probe/reset, NIC edits, gNB reads or transmitter.
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=$REPO/cmake_targets/ran_build/build
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test "$(git -C "$REPO" branch --show-current)" = adaptive-rx-UL-DL
test -x "$BUILD/nr-uesoftmodem"
# The binary must not predate the tree. Measured 2026-09-09: two admissibility rules were added and
# only the TEST targets were rebuilt, so a 40-minute capture ran the previous nr-uesoftmodem and
# reported the pre-fix hypothesis count. Nothing in the recorded commit/patch/sha256 identity catches
# that -- they describe the SOURCE, and the source was correct; it was the binary that was behind.
if find "$REPO/openair1" "$REPO/openair2" "$REPO/executables" "$REPO/radio"      \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' \)      -newer "$BUILD/nr-uesoftmodem" -print -quit 2>/dev/null | grep -q .; then
  echo "BLOCKED: nr-uesoftmodem is older than tracked source -- rebuild before capturing"; exit 3
fi
grep -qx "ENABLE_ISAC_SENSING:BOOL=OFF" "$BUILD/CMakeCache.txt"
test "$(readlink "$BUILD/liboai_device.so")" = liboai_usrpdevif.so
exec 9>/tmp/adaptive-rx-UL-DL.radio.lock
flock -n 9 || { echo "BLOCKED: another adaptive test holds lock"; exit 3; }
if pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null; then
  echo "BLOCKED: a modem is already running"; exit 3
fi
if pgrep -f 'uhd_usrp_probe|benchmark_rate|rx_samples_to_file|rx_multi_samples' >/dev/null; then
  echo "BLOCKED: another radio utility is running"; exit 3
fi
test "$(cat /sys/class/net/enp129s0f0np0/mtu)" = 9000
ip -4 addr show enp129s0f0np0 | grep -q '192.168.20.1/24'
ethtool -g enp129s0f0np0 | awk '/Current hardware/{seen=1} seen && /^RX:/{exit $2 != 8192}'
sudo -n true
DISCOVERY=$(timeout 10s uhd_find_devices --args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174,serial=327C1F2 2>&1)
printf '%s\n' "$DISCOVERY" | grep -q 'claimed: False' || { echo "BLOCKED: X410 does not report unclaimed"; exit 3; }
OUT=$(mktemp -d /home/sens/NICOLA/captures/adaptive_ul_dl_mrc2.XXXXXX)
# CONF selects the receiver config, so a diagnostic variant does not need a copy of this
# script -- copying it elsewhere breaks the repo-root derivation and every guard above it.
cp -- "$SCRIPT_DIR/${CONF:-adaptive_no_hints.conf}" "$OUT/receiver.conf"
git -C "$REPO" rev-parse HEAD > "$OUT/source_commit.txt"
git -C "$REPO" diff --binary > "$OUT/source.patch"
(cd "$REPO" && git ls-files -z --cached --others --exclude-standard -- openair1 openair2 executables radio tests/passive_rx |
  while IFS= read -r -d '' source_file; do
    if [ -f "$source_file" ]; then sha256sum -- "$source_file"; fi
  done) > "$OUT/source_files.sha256"
sha256sum "$BUILD/nr-uesoftmodem" "$BUILD/liboai_usrpdevif.so" "$OUT/receiver.conf" > "$OUT/checksums.txt"
printf '%s\n' "${DURATION:-480}" > "$OUT/duration_s.txt"
printf 'MRC=%s UL_BRANCH=%s RXGAIN=%s CONF=%s\n' "${MRC:-2}" "${UL_BRANCH:-unset}" "${RXGAIN:-40}" "${CONF:-adaptive_no_hints.conf}" > "$OUT/arm.txt"
cat /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors > "$OUT/nic_missed_before.txt"
echo "OUTPUT=$OUT"
cd "$BUILD"
# CFO seed from the preceding passive-receiver measurement, not a gNB hint.
# Upstream continuous compensation is distinct from disabled DM-RS/SFO feedback.
# ---- ACQUISITION VALIDITY WATCHDOG -------------------------------------------------------------
# Roughly half the captures on this rig mis-lock the CFO at acquisition. The receiver still looks
# healthy -- PBCH decodes 50/50, no RFSTALL, no NIC loss -- but SIB1 never decodes, so the blind
# monitor never leaves CORESET#0 and re-derives the CSS0 config every slot forever. Measured
# 2026-09-09: a VOID run settled at CFO -24334 Hz with zero "SIB1 common facts" lines and 474,202
# CSS0-autoconf lines in 4 minutes, against -14965 Hz / 1 / 284 on the VALID run beside it.
#
# So SIB1 is the acquisition go/no-go, and it lands in the first seconds or not at all. Probe for
# it, and on failure kill and RETRY rather than spending the whole DURATION on a dead capture.
# Deliberately NOT a live CFO retune: doing that killed the radio 2/2 times previously.
# Each void attempt's log is kept as evidence, never silently discarded.
run_modem() {
  sudo -n env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    ISAC_RX_MRC_MODE="${MRC:-2}" ISAC_UL_RX_BRANCH="${UL_BRANCH:-}" \
    ISAC_DMRS_FO_APPLY=0 ISAC_SFO_CORRECT=0 \
    ISAC_RX_BRANCH_FO=0 ISAC_RX_GAIN_TRIM=0,0,0,0 \
    ISAC_DISC_NO_RESYNC=0 ISAC_RF_STALL_MAX_REINIT=0 ISAC_CFO_TRACK_HZ=1 ISAC_CFO_TRACK_PERIOD=20 \
    ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
    ISAC_UL_TA_SWEEP=0:0:0 ISAC_SENSE_COMB=0 ISAC_TSYNC_RESET=0 \
    ISAC_PASSIVE_REPLAY_CAPTURE="$OUT/replay.bin" ISAC_PASSIVE_REPLAY_FAILURES=1 \
    LD_LIBRARY_PATH="$BUILD:/usr/local/lib" \
    timeout --signal=TERM --kill-after=10s "${DURATION:-480}s" taskset -c 0-7 "$BUILD/nr-uesoftmodem" \
    --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
    -O "$OUT/receiver.conf" -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
    --ue-rxgain ${RXGAIN:-40} --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx \
    --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 --freq-sync-I 0.001 \
    --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
    --ntn-initial-time-drift -4.25 -A 90 > "$OUT/run.log" 2>&1
}

ACQ_TIMEOUT_S=${ACQ_TIMEOUT_S:-120}
ATTEMPTS=${ATTEMPTS:-4}
rc=1
verdict=VOID_NO_SIB1
set +e
for attempt in $(seq 1 "$ATTEMPTS"); do
  : > "$OUT/run.log"
  run_modem &
  modem_wait=$!
  acquired=0
  for _ in $(seq 1 "$((ACQ_TIMEOUT_S / 5))"); do
    sleep 5
    if grep -qa 'SIB1 common facts' "$OUT/run.log" 2>/dev/null; then acquired=1; break; fi
    kill -0 "$modem_wait" 2>/dev/null || break
  done
  if [ "$acquired" = 1 ]; then
    echo "attempt $attempt: SIB1 acquired, running the full ${DURATION:-480}s"
    wait "$modem_wait"; rc=$?
    # Acquiring is not surviving. A healthy run is ended by `timeout`, i.e. exit 124; anything else
    # is the receiver dying early. Measured 2026-09-09: an AssertFatal inside get_dmrs_port() aborted
    # a capture (exit 134, core dumped) minutes in, and this script still called it VALID because it
    # only looked at SIB1. A crashed run reported as VALID is worse than no run at all.
    if [ "$rc" = 124 ]; then verdict=VALID; else verdict="VOID_ABNORMAL_EXIT_$rc"; fi
    break
  fi
  cfo=$(grep -aoE 'current=-?[0-9]+' "$OUT/run.log" | tail -1)
  echo "attempt $attempt VOID: no SIB1 within ${ACQ_TIMEOUT_S}s (${cfo:-cfo=?}); retrying acquisition"
  cp -- "$OUT/run.log" "$OUT/void_attempt${attempt}.log"
  # Resolve our exact receiver by its unique copied config path. Never signal
  # an unrelated modem that happens to share the executable name.
  for candidate in $(pgrep -x nr-uesoftmodem); do
    if sudo -n sh -c 'tr "\000" "\n" < "$1"' sh "/proc/$candidate/cmdline" 2>/dev/null |
        grep -Fxq -- "$OUT/receiver.conf"; then
      sudo -n kill -TERM "$candidate" 2>/dev/null
    fi
  done
  wait "$modem_wait" 2>/dev/null
  sleep 5
done
set -e
printf '%s\n' "$rc" > "$OUT/process_exit.txt"
cat /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors > "$OUT/nic_missed_after.txt"
if [ "$verdict" = VALID ]; then
  if ! cmp -s "$OUT/nic_missed_before.txt" "$OUT/nic_missed_after.txt"; then
    verdict=VOID_NIC_MISSED
  elif grep -qaE 'RXDISCONT|RFSTALL' "$OUT/run.log"; then
    verdict=VOID_RF_DISCONTINUITY
  fi
fi
printf '%s\n' "$verdict" > "$OUT/validity.txt"
echo "verdict=$verdict exit=$rc; inspect receiver evidence before any further verdict. OUTPUT=$OUT"
