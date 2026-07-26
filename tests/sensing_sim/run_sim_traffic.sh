#!/bin/bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Traffic-injection variant of run_sim.sh: launches the same core-less gNB/UE sensing-sim pair,
# but with --noS1 on both sides (so each gets a local TUN: oaitun_enb1/10.0.1.1,
# oaitun_ue1/10.0.1.2) and a `ping` driven across them so real DL PDSCH actually flows -- letting
# the [sensing] sources="csi_rs,pdsch_dmrs,pdsch_data" fusion in ue.sensing.traffic.rfsim.conf do
# something.
#
# Per doc/runmodem-nrue.md's explicit warning ("this does not work if both interfaces are on the
# same host... use network namespaces"), each process runs in its OWN netns, connected by a veth
# pair carrying the rfsimulator TCP loopback (192.168.100.1/.2); each process's own noS1 TUN
# (10.0.1.1/10.0.1.2) lives inside its own netns, so there's no cross-namespace routing ambiguity.
# Requires sudo (netns/veth/TUN creation needs CAP_NET_ADMIN).
#
# Usage: sudo ./run_sim_traffic.sh [duration_s] [out_dir]
set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../../cmake_targets/ran_build/build}"
DURATION="${1:-90}"
OUT_DIR="${2:-/tmp/sensing_sim_traffic}"
UE_CONF="${3:-ue.sensing.traffic.rfsim.conf}"

NS_GNB=oai_isac_gnb
NS_UE=oai_isac_ue
VETH_GNB=veth-isac-g
VETH_UE=veth-isac-u
GNB_IP=192.168.100.1
UE_IP=192.168.100.2

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

mkdir -p "$OUT_DIR"
GNB_LOG="$OUT_DIR/gnb.log"
UE_LOG="$OUT_DIR/ue.log"
PING_LOG="$OUT_DIR/ping.log"

cleanup() {
  echo "Cleaning up..."
  $SUDO pkill -9 -f "nr-softmodem.*$NS_GNB" 2>/dev/null || true
  [ -n "${GNB_PID:-}" ] && $SUDO kill -9 "$GNB_PID" 2>/dev/null || true
  [ -n "${UE_PID:-}" ] && $SUDO kill -9 "$UE_PID" 2>/dev/null || true
  [ -n "${PING_PID:-}" ] && $SUDO kill "$PING_PID" 2>/dev/null || true
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

$SUDO ip netns exec "$NS_GNB" "$BUILD_DIR/nr-softmodem" \
  -O "$SCRIPT_DIR/gnb.sensing.rfsim.conf" --do-ra --rfsim --noS1 >"$GNB_LOG" 2>&1 &
GNB_PID=$!

sleep 3 # let the gNB open the rfsim server socket + its TUN before the UE connects

$SUDO ip netns exec "$NS_UE" "$BUILD_DIR/nr-uesoftmodem" \
  -O "$SCRIPT_DIR/$UE_CONF" --do-ra --rfsim --noS1 \
  -r 106 --numerology 1 --band 78 -C 3619080000 \
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

echo "Starting ping (gNB oaitun_enb1 -> UE 10.0.1.2) for ${DURATION}s..."
$SUDO ip netns exec "$NS_GNB" ping -I oaitun_enb1 -i 0.2 10.0.1.2 >"$PING_LOG" 2>&1 &
PING_PID=$!

sleep "$DURATION"

echo "Stopping..."
$SUDO kill "$PING_PID" 2>/dev/null || true
$SUDO kill "$GNB_PID" "$UE_PID" 2>/dev/null || true
sleep 2
$SUDO kill -9 "$GNB_PID" "$UE_PID" 2>/dev/null || true
trap - EXIT
$SUDO ip netns del "$NS_GNB" 2>/dev/null || true
$SUDO ip netns del "$NS_UE" 2>/dev/null || true

echo ""
echo "=== RA outcome ==="
grep -m1 "RA procedure succeeded" "$UE_LOG" || echo "(not found -- check $UE_LOG)"
echo ""
echo "=== Ping stats ==="
tail -5 "$PING_LOG" || echo "(no ping log)"
echo ""
echo "=== PDSCH occurrence check (UE log) ==="
grep -c "nr_ue_pdsch_procedures\|dlsch.*decod\|PDSCH.*decod" "$UE_LOG" 2>/dev/null || echo "0"
echo ""
echo "=== Ground truth (sensing_channel, UE log) ==="
grep "SENSING_CHANNEL gt:" "$UE_LOG" | tail -5
echo ""
echo "=== Detections (sensing report, UE log) ==="
grep "SENSING: CPI #" "$UE_LOG" | tail -5
echo ""
echo "Full logs: $GNB_LOG / $UE_LOG / $PING_LOG"
echo "DetectionReport JSON-lines (if any): $OUT_DIR/oaiue_reports.jsonl"
