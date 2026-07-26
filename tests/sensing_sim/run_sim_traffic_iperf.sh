#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Heavy-DL variant of run_sim_traffic.sh: same core-less gNB/UE sensing-sim pair over netns +
# veth (see that script's header for the noS1/netns rationale -- unchanged here), but drives
# steady iperf3 UDP traffic (gNB -> UE, i.e. DL) instead of a sparse ping, so PDSCH gets scheduled
# far more densely and regularly. Motivation (2026-07-22 ECA+ validation session): with ping-only
# traffic, pdsch_data occurrences were sparse and irregularly spaced (T_slot swinging ~24-33 slots
# CPI to CPI), which (a) starved the STO/CFO/SFO trackers of enough regularly-spaced samples to
# converge (observed rms_rad close to pi, CFO/SFO estimates flipping sign CPI to CPI -- fitting
# noise, not a real residual) and (b) forced cpi_slots down from the designed 256 to 64 just to
# close a CPI inside a practical test duration. Denser DL traffic should fix both without needing
# to shrink cpi_slots.
#
# Uses its own netns/veth names (oai_isac_gnb2/oai_isac_ue2, veth-isac-g2/-u2) so it can run
# alongside (not colliding with) run_sim_traffic.sh's netns if one is still lingering.
#
# Usage: sudo ./run_sim_traffic_iperf.sh [duration_s] [out_dir] [ue_conf] [iperf_bitrate]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../../cmake_targets/ran_build/build}"
DURATION="${1:-300}"
OUT_DIR="${2:-/tmp/sensing_sim_traffic_iperf}"
UE_CONF="${3:-ue.sensing.traffic.moving.eca.rfsim.conf}"
IPERF_BW="${4:-3M}"
# Carrier geometry, overridable so the 100 MHz (273 PRB) variant can reuse this runner unchanged.
# Defaults are the original 40 MHz / 106 PRB setup. When overriding, GNB_CONF/UE_PRB/UE_CFREQ must
# agree: the UE centre frequency is pointA + N_RB*12*SCS/2 for the gNB conf in use.
GNB_CONF="${GNB_CONF:-gnb.sensing.rfsim.conf}"
UE_PRB="${UE_PRB:-106}"
UE_CFREQ="${UE_CFREQ:-3619080000}"

NS_GNB=oai_isac_gnb2
NS_UE=oai_isac_ue2
VETH_GNB=veth-isac-g2
VETH_UE=veth-isac-u2
GNB_IP=192.168.101.1
UE_IP=192.168.101.2
UE_TUN_IP=10.0.1.2

if [ "$(id -u)" -ne 0 ] && ! sudo -n true 2>/dev/null; then
  echo "ERROR: this script needs root (netns/veth/TUN creation). Run with sudo or ensure passwordless sudo." >&2
  exit 1
fi
SUDO="sudo"

for exe in nr-softmodem nr-uesoftmodem; do
  if [ ! -x "$BUILD_DIR/$exe" ]; then
    echo "ERROR: $BUILD_DIR/$exe not found. Build it first." >&2
    exit 1
  fi
done
if ! command -v iperf3 >/dev/null 2>&1; then
  echo "ERROR: iperf3 not found on PATH." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
GNB_LOG="$OUT_DIR/gnb.log"
UE_LOG="$OUT_DIR/ue.log"
IPERF_SERVER_LOG="$OUT_DIR/iperf_server.log"
IPERF_CLIENT_LOG="$OUT_DIR/iperf_client.log"

cleanup() {
  echo "Cleaning up..."
  [ -n "${IPERF_C_PID:-}" ] && $SUDO kill -9 "$IPERF_C_PID" 2>/dev/null || true
  [ -n "${IPERF_S_PID:-}" ] && $SUDO kill -9 "$IPERF_S_PID" 2>/dev/null || true
  [ -n "${GNB_PID:-}" ] && $SUDO kill -9 "$GNB_PID" 2>/dev/null || true
  [ -n "${UE_PID:-}" ] && $SUDO kill -9 "$UE_PID" 2>/dev/null || true
  $SUDO ip netns del "$NS_GNB" 2>/dev/null || true
  $SUDO ip netns del "$NS_UE" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== Setting up network namespaces + veth pair ==="
$SUDO ip netns del "$NS_GNB" 2>/dev/null || true
$SUDO ip netns del "$NS_UE" 2>/dev/null || true
$SUDO ip netns add "$NS_GNB"
$SUDO ip netns add "$NS_UE"
$SUDO ip link add "$VETH_GNB" type veth peer name "$VETH_UE"
$SUDO ip link set "$VETH_GNB" netns "$NS_GNB"
$SUDO ip link set "$VETH_UE" netns "$NS_UE"
$SUDO ip netns exec "$NS_GNB" ip addr add "$GNB_IP/24" dev "$VETH_GNB"
$SUDO ip netns exec "$NS_UE"  ip addr add "$UE_IP/24" dev "$VETH_UE"
$SUDO ip netns exec "$NS_GNB" ip link set "$VETH_GNB" up
$SUDO ip netns exec "$NS_UE"  ip link set "$VETH_UE" up
$SUDO ip netns exec "$NS_GNB" ip link set lo up
$SUDO ip netns exec "$NS_UE"  ip link set lo up
echo "netns ready: $NS_GNB ($GNB_IP) <-> $NS_UE ($UE_IP)"

echo "gNB log:  $GNB_LOG"
echo "UE log:   $UE_LOG"
echo "Duration: ${DURATION}s"
echo "iperf3 DL bitrate target: $IPERF_BW (UDP, gNB -> UE)"
echo "gNB conf: $GNB_CONF   UE: -r $UE_PRB -C $UE_CFREQ"

$SUDO ip netns exec "$NS_GNB" "$BUILD_DIR/nr-softmodem" \
  -O "$SCRIPT_DIR/$GNB_CONF" --do-ra --rfsim --noS1 >"$GNB_LOG" 2>&1 &
GNB_PID=$!

sleep 3 # let the gNB open the rfsim server socket + its TUN before the UE connects

$SUDO ip netns exec "$NS_UE" env ISAC_DBG_DT="${ISAC_DBG_DT:-}" "$BUILD_DIR/nr-uesoftmodem" \
  -O "$SCRIPT_DIR/$UE_CONF" --do-ra --rfsim --noS1 \
  -r "$UE_PRB" --numerology 1 --band 78 -C "$UE_CFREQ" \
  --rfsimulator.[0].serveraddr "$GNB_IP" >"$UE_LOG" 2>&1 &
UE_PID=$!

echo "gNB PID=$GNB_PID  UE PID=$UE_PID"

echo "Waiting for RA + both noS1 TUN interfaces to come up..."
TUN_WAIT=0
while [ "$TUN_WAIT" -lt 60 ]; do
  if $SUDO ip netns exec "$NS_GNB" ip link show oaitun_enb1 >/dev/null 2>&1 && \
     $SUDO ip netns exec "$NS_UE"  ip link show oaitun_ue1  >/dev/null 2>&1; then
    echo "Both TUN interfaces up after ${TUN_WAIT}s."
    break
  fi
  sleep 2
  TUN_WAIT=$((TUN_WAIT + 2))
done
if [ "$TUN_WAIT" -ge 60 ]; then
  echo "WARNING: TUN interfaces did not appear within 60s -- traffic will not flow. Continuing anyway."
fi

echo "Starting iperf3 server on UE ($UE_TUN_IP)..."
$SUDO ip netns exec "$NS_UE" iperf3 -s -B "$UE_TUN_IP" -1 >"$IPERF_SERVER_LOG" 2>&1 &
IPERF_S_PID=$!
sleep 2 # let the server bind before the client connects

IPERF_T=$((DURATION - 5))
[ "$IPERF_T" -lt 1 ] && IPERF_T=1
echo "Starting iperf3 UDP client on gNB -> $UE_TUN_IP for ${IPERF_T}s..."
$SUDO ip netns exec "$NS_GNB" iperf3 -c "$UE_TUN_IP" -u -b "$IPERF_BW" -t "$IPERF_T" -i 10 >"$IPERF_CLIENT_LOG" 2>&1 &
IPERF_C_PID=$!

sleep "$DURATION"

echo "Stopping..."
$SUDO kill "$IPERF_C_PID" "$IPERF_S_PID" 2>/dev/null || true
$SUDO kill "$GNB_PID" "$UE_PID" 2>/dev/null || true
sleep 2
$SUDO kill -9 "$IPERF_C_PID" "$IPERF_S_PID" "$GNB_PID" "$UE_PID" 2>/dev/null || true
trap - EXIT
$SUDO ip netns del "$NS_GNB" 2>/dev/null || true
$SUDO ip netns del "$NS_UE" 2>/dev/null || true

echo ""
echo "=== RA outcome ==="
grep -m1 "RA procedure succeeded" "$UE_LOG" || echo "(not found -- check $UE_LOG)"
echo ""
echo "=== iperf3 client summary ==="
tail -15 "$IPERF_CLIENT_LOG" || echo "(no iperf client log)"
echo ""
echo "=== Ground truth (sensing_channel, UE log) ==="
grep "SENSING_CHANNEL gt:" "$UE_LOG" | tail -5
echo ""
echo "=== Detections (sensing report, UE log) ==="
grep "SENSING: CPI #" "$UE_LOG" | tail -8
echo ""
echo "Full logs: $GNB_LOG / $UE_LOG / $IPERF_SERVER_LOG / $IPERF_CLIENT_LOG"
echo "DetectionReport JSON-lines (if any): $OUT_DIR/oaiue_reports.jsonl"
