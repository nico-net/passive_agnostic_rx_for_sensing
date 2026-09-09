#!/usr/bin/env bash
# Bounded receiver-only run. No pkill, probe/reset, NIC edits, gNB reads or transmitter.
set -euo pipefail
REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD=$REPO/cmake_targets/ran_build/build
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test "$(git -C "$REPO" branch --show-current)" = adaptive-rx-UL-DL
test -x "$BUILD/nr-uesoftmodem"
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
cp -- "$SCRIPT_DIR/adaptive_no_hints.conf" "$OUT/receiver.conf"
git -C "$REPO" rev-parse HEAD > "$OUT/source_commit.txt"
git -C "$REPO" diff --binary > "$OUT/source.patch"
sha256sum "$BUILD/nr-uesoftmodem" "$BUILD/liboai_usrpdevif.so" "$OUT/receiver.conf" > "$OUT/checksums.txt"
printf '%s\n' "${DURATION:-480}" > "$OUT/duration_s.txt"
cat /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors > "$OUT/nic_missed_before.txt"
echo "OUTPUT=$OUT"
cd "$BUILD"
# CFO seed from the preceding passive-receiver measurement, not a gNB hint.
# Upstream continuous compensation is distinct from disabled DM-RS/SFO feedback.
set +e
sudo -n env -i PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin ISAC_RX_MRC_MODE=2 ISAC_DMRS_FO_APPLY=0 ISAC_SFO_CORRECT=0 \
  ISAC_RX_BRANCH_FO=0 ISAC_RX_GAIN_TRIM=0,0,0,0 \
  ISAC_DISC_NO_RESYNC=0 ISAC_RF_STALL_MAX_REINIT=0 ISAC_CFO_TRACK_HZ=1 ISAC_CFO_TRACK_PERIOD=20 \
  ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
  ISAC_UL_TA_SWEEP=0:0:0 ISAC_SENSE_COMB=0 ISAC_TSYNC_RESET=0 \
  ISAC_PASSIVE_REPLAY_CAPTURE="$OUT/replay.bin" \
  LD_LIBRARY_PATH="$BUILD:/usr/local/lib" \
  timeout --signal=TERM --kill-after=10s "${DURATION:-480}s" taskset -c 0-7 "$BUILD/nr-uesoftmodem" \
  --usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
  -O "$OUT/receiver.conf" -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
  --ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx \
  --ue-fo-compensation --cont-fo-comp 1 --freq-sync-P 0.05 --freq-sync-I 0.001 \
  --initial-fo -16480 --thread-pool 0,1,6,7 --time-sync-I 0.01 \
  --ntn-initial-time-drift -4.25 -A 90 > "$OUT/run.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$OUT/process_exit.txt"
cat /sys/class/net/enp129s0f0np0/statistics/rx_missed_errors > "$OUT/nic_missed_after.txt"
echo "Process exit=$rc; inspect receiver evidence before any verdict. OUTPUT=$OUT"
