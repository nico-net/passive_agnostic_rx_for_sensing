#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Thin BWP-switch wrapper around run_passive_rx.sh (OAI SA bed with a core; a --phy-test gNB cannot
# switch: its test UE has no F1/RRC context, so trigger_bwp_switch has no reconfiguration to send).
# It only adds the gNB's telnet CI module, fires ONE trigger_bwp_switch mid-run, and splits the
# passive receiver's log at the trigger so post-switch evidence cannot come from before it.
# The gNB's telnet replies are validation truth only; nothing is fed to the receiver.
#
# usage: run_bwp_switch.sh <baseline|switch> [duration_s] [out_dir]
# env:   TRIGGER_AT (s, default duration/2)  BWP_TARGET (default 2)  TELNET_PORT (default 9090)
#        RUNNER (default ./run_passive_rx.sh); run_passive_rx.sh env (NUM_UE, TRAFFIC, ...) passes through.
set -u
MODE=${1:-}; DUR=${2:-300}; OUT=${3:-/tmp/passive_rx_bwp}
case "$MODE" in baseline|switch) ;; *) echo "usage: $0 <baseline|switch> [duration_s] [out_dir]" >&2; exit 2;; esac
D=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RUNNER=${RUNNER:-$D/run_passive_rx.sh}
AT=${TRIGGER_AT:-$((DUR / 2))}; PORT=${TELNET_PORT:-9090}; TGT=${BWP_TARGET:-2}
export GNB_CONF_OVERRIDE=${GNB_CONF_OVERRIDE:-gnb.sa.rfsim.bwp.conf} CONF_TAG=${CONF_TAG:-.bwp.agn}
export NUM_RX=${NUM_RX:-1} NUM_UE=${NUM_UE:-1}
export ISAC_BWP_TRACK=${ISAC_BWP_TRACK:-1}   # passive BWP tracker (the receiver feature under test)
export GNB_EXTRA="${GNB_EXTRA:-} --telnetsrv --telnetsrv.listenport $PORT --telnetsrv.shrmod ci"
mkdir -p "$OUT"
RXLOG=$OUT/ue_rx1.log

# One CI command per connection (telnetsrv sends no greeting; reply read for 3 s).
ci() {
  exec 3<>"/dev/tcp/127.0.0.1/$PORT" || { echo "telnet connect failed: $1"; return 1; }
  printf 'ci %s\n' "$1" >&3
  timeout 3 cat <&3
  exec 3<&-
}

"$RUNNER" "$DUR" "$OUT" >"$OUT/runner.log" 2>&1 &
RP=$!
sleep "$AT"
OFF=$(stat -c %s "$RXLOG" 2>/dev/null || echo 0)   # byte boundary: everything after it is post-trigger
echo "$OFF" >"$OUT/trigger_offset"
{
  echo "== $(date -Is) before"; ci get_current_bwp
  [ "$MODE" = switch ] && { echo "== $(date -Is) trigger"; ci "trigger_bwp_switch $TGT"; }
  sleep 5; echo "== $(date -Is) after"; ci get_current_bwp
} >>"$OUT/telnet.log" 2>&1
wait "$RP"; RC=$?

# Scoring (grep only): pre = bytes before the trigger, post = after it; a line the boundary splits
# belongs to neither side.
[ -f "$RXLOG" ] || { echo "no receiver log $RXLOG (runner rc=$RC)" >&2; exit $((RC ? RC : 1)); }
CUT=0
[ "$OFF" -gt 0 ] && [ -n "$(tail -c +"$OFF" "$RXLOG" | head -c 1)" ] && CUT=1
pre() { head -c "$OFF" "$RXLOG" | head -n -"$CUT"; }
post() { tail -c +"$((OFF + 1))" "$RXLOG" | tail -n +"$((CUT + 1))"; }
last_crc() { grep -o 'pdsch_decode\[try=[0-9]* crc_ok=[0-9]*' | tail -1 | grep -o '[0-9]*$'; }
last_census() { grep 'PDSCHQ per-rnti' | tail -1 | grep -o '0x[0-9a-fA-F]*:[0-9]*/[0-9]*' | tr '\n' ' '; }
TRIG=1
[ "$MODE" = switch ] && { grep -q 'triggered BWP switch' "$OUT/telnet.log" || TRIG=0; }
{
  echo "mode=$MODE trigger_at=${AT}s offset=$OFF runner_rc=$RC trigger_ok=$TRIG"
  echo "armed=$(grep -c 'SENSING: BWP tracking armed' "$RXLOG")"
  echo "switch_pre=$(pre | grep -c 'SENSING: BWP SWITCH')"
  echo "switch_post=$(post | grep -c 'SENSING: BWP SWITCH')"
  echo "new_post=$(post | grep -c 'SENSING: BWP NEW')"
  echo "resolved_post=$(post | grep -c 'SENSING: BWP RESOLVED')"
  echo "crc_ok_pre=$(pre | last_crc)"
  echo "crc_ok_post=$(post | last_crc)"
  echo "rnti_census_pre=$(pre | last_census)"
  echo "rnti_census_post=$(post | last_census)"
} | tee "$OUT/bwp_summary.txt"
[ "$RC" -ne 0 ] && exit "$RC"
[ "$TRIG" -eq 1 ] || exit 1
