#!/bin/bash
# M1 end-to-end: rfsim gNB -> publisher UE (rfsim, ISAC_SHM_IQ_PUB=ring, decodes its own cell) -> N consumer UEs
# (device shm_iq, NO --rfsim) on the same ring. Logs under OUT. Stops everything with SIGINT only.
# usage: [BUILD=.. GNBCONF=.. RXCONF=.. CELL=".." GNBARGS=".." RXEXTRA=".." RING=mpci_ring PIN=0|1 CONSUMER_DELAY=20 NICE_PUB=0 PUBEXTRA=""] \
#        rfsim_shm_fanout.sh <OUT> <secs> [N_consumers=1]
# NICE_PUB=19 gives the consumers CPU priority over the publisher (4-core hosts: gNB + 2 UEs oversubscribe).
# Run under `flock /tmp/claude-0/ninja.lock` (no builds while receivers run). PIN=1: taskset gNB=$GNBCORES(0), pub=$PUBCORES(1), consumers=$CONCORES(2-3).
# Consumer i (1..N): tag c<i>, cwd OUT/c<i>, own metrics/obs; cfg = RXCONF + device/loader lines (OUT/consumer.cfg).
set -u
R=$(cd "$(dirname "$0")/../../.." && pwd); B=${BUILD:-$R/cmake_targets/ran_build/build}
OUT=$(realpath -m "$1"); DUR=$2; N=${3:-1}; RING=${RING:-mpci_ring}
: "${GNBCONF:=$R/tests/passive_rx/gnb.sa.rfsim.conf}" "${RXCONF:=$R/tests/passive_rx/ue.passive.bwp.agn.conf}"
: "${CELL:=-C 3319680000 -r 106 --ssb 516}" "${GNBARGS:=-m 9 -n 0 -M 106 -l 1}" "${RXEXTRA:=}" "${PIN:=0}" "${PUBEXTRA:=}" "${CONSUMER_DELAY:=20}"
: "${GNBCORES:=0}" "${PUBCORES:=1}" "${CONCORES:=2-3}"
: "${V4SHIM:=$([ -e /proc/net/if_inet6 ] && echo 0 || echo 1)}"
[ "$(nproc)" -le 5 ] && RXEXTRA="$RXEXTRA --sensing.pdcch_blind_monitor_scan_thread 1:8:-1"  # frozen cfg pins core 5
pgrep -x nr-uesoftmodem >/dev/null || pgrep -x nr-softmodem >/dev/null && { echo "another softmodem is running" >&2; exit 2; }
rm -rf "$OUT" "/dev/shm/$RING"; mkdir -p "$OUT/gnb" "$OUT/pub"; rm -rf /tmp/passive_rx; mkdir -p /tmp/passive_rx
{ cat "$RXCONF"; echo 'device = { name = "shm_iq"; };'; echo "loader = { shm_iq = { shlibpath = \"$B\"; }; };"; } > "$OUT/consumer.cfg"
pin() { [ "$PIN" = 1 ] && echo "taskset -c $1"; }
GNBENV=(); if [ "$V4SHIM" = 1 ]; then
  gcc -shared -fPIC -O2 -o "$OUT/v4only_shim.so" "$R/tests/passive_rx/dgx/v4only_shim.c" -ldl || exit 2; GNBENV=(env "LD_PRELOAD=$OUT/v4only_shim.so"); fi
PIDS=()
( cd "$OUT/gnb" && touch nrL1_stats.log nrMAC_stats.log nr_stats.log &&
  exec $(pin "$GNBCORES") timeout -s INT $((DUR+CONSUMER_DELAY+30)) "${GNBENV[@]}" "$B/nr-softmodem" --phy-test --noS1 -D 0xff -O "$GNBCONF" --rfsim $GNBARGS > gnb.log 2>&1 ) &
GP=$!; sleep 10
# run_rx <dir> <cores> <secs> <VAR=val>... -- <args>: SIGINT timeout; CPU seconds (bash `times` of the finished child) -> dir/cpu.txt
run_rx() { local d=$1 c=$2 t=$3; shift 3; local e=(); while [ "$1" != -- ]; do e+=("$1"); shift; done; shift
  ( cd "$d" && touch nrL1_stats.log nr_stats.log && env ISAC_CORE_PLAN=0 ISAC_MLOCK=0 "${e[@]}" $([ "$PIN" = 1 ] && echo taskset -c "$c") \
      nice -n "${NICEV:-0}" timeout -s INT "$t" "$B/nr-uesoftmodem" --passive-rx "$@" > rx.log 2>&1; echo "rc=$?" > rc.txt; times > cpu.all; tail -1 cpu.all > cpu.txt ) & }
NICEV=${NICE_PUB:-0} run_rx "$OUT/pub" "$PUBCORES" $((DUR+CONSUMER_DELAY)) ISAC_SHM_IQ_PUB=$RING ISAC_METRICS_PATH=$OUT/pub/metrics.jsonl ISAC_CELL_INSTANCE=pub -- \
  --rfsim -O "$RXCONF" $CELL --numerology 1 --band 78 $RXEXTRA $PUBEXTRA
PP=$!
for _ in $(seq 1 60); do [ -e "/dev/shm/$RING" ] && break; sleep 1; done
sleep "$CONSUMER_DELAY"   # let the publisher reach sync/tracking before the consumers attach
CP=()
for i in $(seq 1 "$N"); do d=$OUT/c$i; mkdir -p "$d"
  run_rx "$d" "$CONCORES" "$DUR" ISAC_SHM_IQ_NAME=$RING ISAC_SHM_IQ_CELL=c$i ISAC_CELL_INSTANCE=c$i ISAC_METRICS_PATH=$d/metrics.jsonl \
    ISAC_UECTX_PATH=$d/uectx.json ISAC_OBS_PATH=$d/obs.bin -- -O "$OUT/consumer.cfg" $CELL --numerology 1 --band 78 $RXEXTRA
  CP+=($!); done
( sleep $((DUR*2/3)); ps -C nr-uesoftmodem,nr-softmodem -o pid,pcpu,rss,args | cut -c1-110 > "$OUT/ps_mid.txt"; top -b -n1 -w 200 | head -12 >> "$OUT/ps_mid.txt" ) &
wait "${CP[@]}"; kill -INT $PP 2>/dev/null; wait $PP; kill -INT $GP 2>/dev/null; wait $GP
# summary: sync, MIB/SIB1, PDSCHQ, SHMIQ per process
for p in pub $(seq -f 'c%g' 1 "$N"); do f=$OUT/$p/rx.log; echo "== $p"
  grep -h -m2 -E "SHMIQ_(PUB_)?READY|SHMIQ_START|SHMIQ_TUNE" "$f"
  grep -h -E "SHMIQ_(OVERRUN|DISCONT|PRODUCER_LOST|END)" "$f" | tail -3
  grep -h -m1 -i -E "initial sync.*(success|found)|PCI|Sync" "$f"; grep -h "PDSCHQ" "$f" | tail -1
  echo "cpu(user sys) $(cat "$OUT/$p/cpu.txt") $(cat "$OUT/$p/rc.txt")"; done
