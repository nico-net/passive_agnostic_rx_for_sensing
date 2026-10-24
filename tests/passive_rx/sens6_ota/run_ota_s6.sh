#!/bin/bash
# sens6 OTA launcher for the passive receiver on the X410 (new 2026-10-24; replaces the FROZEN captures/run_arm.sh for this host,
# which cannot run as-is: it cd's into the missing /home/sens/NICOLA/openairinterface5g-total-passive-ue, SIGKILLs the receiver in
# two watchdogs, claims the X410 with uhd_usrp_probe before every try, narrows the CPUs with taskset/--thread-pool, and changes the
# host (ip/ethtool/sysctl/IRQ) with sudo on its own).
#
# usage:  run_ota_s6.sh check                      read-only preflight; prints every sudo command still needed; never changes the host
#         run_ota_s6.sh dry-run [ARM]              preflight + the exact launch command, nothing started
#         run_ota_s6.sh run ARM                    preflight (must be clean) + one run of DUR seconds, stopped with SIGINT
# env:    ENVFILE (default ota_lab273.s6.env next to this script), CFG (default ue.passive.ota.lab273.s6.cfg), B (build dir,
#         default <worktree>/build_ota), CARRIER PRB SSB SCAN=1 NANT RXG INITIALFO DUR (override the env file), DATA (X410 data
#         address, 192.168.20.2), MGMT (default: what uhd_find_devices reports, else DATA), NIC (enp2s0f1np1), OUTBASE
#         (/home/sens/NICOLA/captures), EXTRA (more receiver arguments), XENV (more KEY=VALUE for the receiver), FORCE=1 (start
#         although the preflight found non-fatal items).
#
# Every run: under flock -x /tmp/td_measure.lock, only with MemAvailable >= 7 GB, inside
#   systemd-run --user --scope -p MemoryMax=min(7G, MemAvailable-0.5G) -p MemorySwapMax=1G choom -n 1000 -- sudo -n env ... timeout -s INT
# sudo is needed only for the real-time scheduling / isolated-CPU pinning of the reader and the radio IO (CAP_SYS_NICE); it was
# measured on sens6 2026-10-24 that a root child of sudo stays in the user scope (MemoryMax holds), keeps oom_score_adj 1000 and gets
# SCHED_FIFO (no RT group scheduling in this kernel). No taskset, no --thread-pool, no ISAC_* core numbers: the core plan
# (SENSING: CORE_PLAN) places everything (sens6 isolcpus 1-2: RT 3, UHD ctrl 1, ring 2, pool 4-9, lane 10-13, housekeeping 0).
# Stop: SIGINT only (timeout -s INT; a second SIGINT after 30 s); never SIGKILL (X410 claim hygiene). A receiver that ignores both
# is reported and left to the operator.
set -u
H=$(cd "$(dirname "$0")" && pwd); R=$(cd "$H/../../.." && pwd)
MODE=${1:-check}; ARM=${2:-}
ENVFILE=${ENVFILE:-$H/ota_lab273.s6.env}
# environment wins over the env file: remember what the caller set
for v in CARRIER PRB SSB NANT RXG INITIALFO DUR; do eval "_o_$v=\${$v:-}"; done
# shellcheck disable=SC1090
[ -r "$ENVFILE" ] && . "$ENVFILE"
for v in CARRIER PRB SSB NANT RXG INITIALFO DUR; do eval "[ -n \"\$_o_$v\" ] && $v=\$_o_$v"; done
CARRIER=${CARRIER:-3450000000}; PRB=${PRB:-273}; SSB=${SSB:-150}; NANT=${NANT:-4}; RXG=${RXG:-49}; INITIALFO=${INITIALFO:--15000}; DUR=${DUR:-300}
CFG=${CFG:-$H/ue.passive.ota.lab273.s6.cfg}; B=${B:-$R/build_ota}; BIN=$B/nr-uesoftmodem
DATA=${DATA:-192.168.20.2}; NIC=${NIC:-enp2s0f1np1}; OUTBASE=${OUTBASE:-/home/sens/NICOLA/captures}
ISSUES=0; FATAL=0; SUDO_CMDS=()
ok()   { echo "  ok    $*"; }
warn() { echo "  WARN  $*"; ISSUES=$((ISSUES+1)); }
bad()  { echo "  FATAL $*"; FATAL=$((FATAL+1)); }
need() { SUDO_CMDS+=("$1"); }
expand_cpus() { echo "$1" | tr ',' '\n' | while IFS=- read -r a b; do seq "$a" "${b:-$a}"; done; }
# the nr-uesoftmodem started by THIS launcher (descendant of pid $1); other agents' receivers are never touched
our_rx() { local p pp; for p in $(pgrep -x nr-uesoftmodem); do pp=$p; while [ "$pp" -gt 1 ]; do [ "$pp" = "$1" ] && { echo "$p"; return; }; pp=$(awk '/^PPid:/{print $2}' /proc/$pp/status 2>/dev/null || echo 1); done; done; }

preflight() {
  echo "== preflight (read-only) $(date '+%F %T') host $(hostname)"
  # receiver / radio users
  if pgrep -x nr-uesoftmodem >/dev/null; then bad "nr-uesoftmodem already running: $(pgrep -x nr-uesoftmodem | tr '\n' ' ')(stop it with SIGINT first)"; else ok "no nr-uesoftmodem running"; fi
  if pgrep -f '[u]hd_(usrp_probe|rx_|benchmark|find)|[b]enchmark_rate|[r]x_samples_to_file' >/dev/null; then bad "a UHD tool is running: $(pgrep -af '[u]hd_|[b]enchmark_rate|[r]x_samples_to_file' | head -2)"; else ok "no UHD tool running"; fi
  [ -n "${ISAC_RX_BRANCH_FO:-}" ] && bad "ISAC_RX_BRANCH_FO is set in the environment (hard rule: must stay unset)"
  grep -q 'ISAC_RX_BRANCH_FO' <<< "${XENV:-}" && bad "XENV sets ISAC_RX_BRANCH_FO (hard rule: must stay unset)"
  # NIC
  local ip; ip=$(ip -4 -br addr show "$NIC" 2>/dev/null | awk '{print $3}')
  if [ -z "$ip" ]; then bad "$NIC has no IPv4 address"; need "sudo ip addr add 192.168.20.1/24 dev $NIC   # or restore the NM profile x410-data"
  else case "$ip" in 192.168.20.*/24) ok "$NIC $ip (X410 subnet 192.168.20.0/24; run_arm.sh expected .1, .65 is equivalent for UHD)";; *) bad "$NIC $ip is not in 192.168.20.0/24";; esac; fi
  [ "$(cat /sys/class/net/$NIC/operstate 2>/dev/null)" = up ] && ok "$NIC link up $(ethtool $NIC 2>/dev/null | awk '/Speed/{print $2}')" || bad "$NIC link not up"
  local mtu; mtu=$(cat /sys/class/net/$NIC/mtu 2>/dev/null)
  [ "$mtu" = 9000 ] && ok "MTU 9000" || { bad "MTU $mtu (need 9000)"; need "sudo ip link set $NIC mtu 9000"; }
  local rx tx; rx=$(ethtool -g "$NIC" 2>/dev/null | awk '/Current hardware/,0' | awk '/^RX:/{print $2}'); tx=$(ethtool -g "$NIC" 2>/dev/null | awk '/Current hardware/,0' | awk '/^TX:/{print $2}')
  [ "$rx" = 8192 ] && [ "$tx" = 8192 ] && ok "rings RX $rx TX $tx" || { warn "rings RX $rx TX $tx (want 8192)"; need "sudo ethtool -G $NIC rx 8192 tx 8192"; }
  # sysctl: UHD needs rmem_max >= its recv_buff_size; run_arm.sh asserted backlog 250000 / rmem_default 62500000
  local rmax bl rdef; rmax=$(sysctl -n net.core.rmem_max); bl=$(sysctl -n net.core.netdev_max_backlog); rdef=$(sysctl -n net.core.rmem_default)
  [ "$rmax" -ge 33554432 ] && ok "rmem_max $rmax" || { warn "rmem_max $rmax (< 32 MB)"; need "sudo sysctl -w net.core.rmem_max=62500000"; }
  if [ "$bl" -ge 250000 ] && [ "$rdef" -ge 62500000 ]; then ok "netdev_max_backlog $bl rmem_default $rdef"
  else warn "netdev_max_backlog $bl rmem_default $rdef (run_arm.sh asserted 250000 / 62500000)"; need "sudo sysctl -w net.core.netdev_max_backlog=250000 net.core.rmem_default=62500000 net.core.rmem_max=62500000"; fi
  # NIC IRQs: keep them off the radio IO (isolated 1-2), the RT reader (3) and the pool (4-9) -> housekeeping 0 + lane 10-13
  local irqs="" badirq=0 i a c
  for i in $(ls /sys/class/net/$NIC/device/msi_irqs 2>/dev/null); do
    a=$(cat /proc/irq/$i/smp_affinity_list 2>/dev/null) || continue; irqs="$irqs $i:$a"
    for c in $(expand_cpus "$a"); do case " 0 10 11 12 13 " in *" $c "*) ;; *) badirq=$((badirq+1)); break;; esac; done
  done
  if [ "$badirq" -eq 0 ]; then ok "NIC IRQs on 0,10-13:$irqs"
  else warn "$badirq NIC IRQs reach radio-IO/RT/pool CPUs 1-9:$irqs"
    need "n=0; for i in \$(ls /sys/class/net/$NIC/device/msi_irqs); do c=\$(echo 0 10 11 12 13 | cut -d' ' -f\$(( n % 5 + 1 ))); echo \$c | sudo tee /proc/irq/\$i/smp_affinity_list >/dev/null; n=\$((n+1)); done   # not reboot-persistent"; fi
  # CPU
  local gov; gov=$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u | tr '\n' ' ')
  [ "$gov" = "performance " ] && ok "governor performance (all CPUs)" || { warn "governor: $gov"; need "echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"; }
  local iso; iso=$(cat /sys/devices/system/cpu/isolated); ok "isolated CPUs: ${iso:-none} (core plan puts UHD ctrl/ring there on air)"
  # memory
  local hp hpf ma; hp=$(awk '/HugePages_Total/{print $2}' /proc/meminfo); hpf=$(awk '/HugePages_Free/{print $2}' /proc/meminfo); ma=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
  if [ "$hp" -gt 0 ]; then warn "hugepages: $hp x $(awk '/Hugepagesize/{print $2" "$3}' /proc/meminfo) reserved, $hpf free; DPDK is not used (kernel sockets), so this RAM is lost to the receiver"
    need "echo 0 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages   # optional: returns the 1 GB hugepages to the receiver until the next reboot"; fi
  [ "$ma" -ge 7000000 ] && ok "MemAvailable $((ma/1024)) MB (>= 7 GB)" || bad "MemAvailable $((ma/1024)) MB < 7 GB: stop other arms/builds first"
  # privilege for RT
  if sudo -n true 2>/dev/null; then ok "passwordless sudo (RT scheduling + isolated-CPU pinning through 'sudo -n env')"
  else bad "sudo -n not available: run 'sudo -v' in this terminal first (RT scheduling needs CAP_SYS_NICE)"; fi
  # binary
  if [ -x "$BIN" ]; then ok "binary $BIN ($(stat -c %y "$BIN" | cut -c1-19))"
    [ -e "$B/liboai_device.so" ] && ok "liboai_device.so -> $(readlink "$B/liboai_device.so")" || bad "$B/liboai_device.so missing (build oai_usrpdevif)"
    local newer; newer=$(find "$R/openair1" "$R/executables" "$R/radio" "$R/common" \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cu' -o -name '*.h' \) -newer "$BIN" -print -quit 2>/dev/null)
    [ -z "$newer" ] && ok "no source newer than the binary" || warn "source newer than the binary ($newer): rebuild, or this run is not the HEAD code"
    ldd "$B/liboai_usrpdevif.so" 2>/dev/null | grep -q '/usr/local/lib/libuhd' && ok "oai_usrpdevif links /usr/local libuhd" || warn "oai_usrpdevif does not link /usr/local/lib/libuhd ($(ldd "$B/liboai_usrpdevif.so" 2>/dev/null | grep libuhd))"
  else bad "binary $BIN missing"; fi
  [ -r "$CFG" ] && ok "cfg $CFG" || bad "cfg $CFG not readable"
  # X410 (no claim: ping + MPM discovery only; uhd_usrp_probe would CLAIM the device and is never run here)
  ping -c1 -W2 "$DATA" >/dev/null 2>&1 && ok "X410 data $DATA answers ping" || bad "X410 data $DATA unreachable over $NIC"
  local disc; disc=$(timeout 20 uhd_find_devices --args "type=x4xx,addr=$DATA" 2>/dev/null)
  if [ -n "$disc" ]; then
    local cl ser fpga mg; cl=$(awk '/claimed:/{print $2}' <<< "$disc" | head -1); ser=$(awk '/serial:/{print $2}' <<< "$disc" | head -1)
    fpga=$(awk '/fpga:/{print $2}' <<< "$disc" | head -1); mg=$(awk '/mgmt_addr:/{print $2}' <<< "$disc" | head -1)
    [ "$cl" = False ] && ok "X410 serial $ser fpga $fpga mgmt_addr $mg claimed: False" || bad "X410 serial $ser claimed: $cl (another session holds it; never start a second claim)"
    MGMT=${MGMT:-$mg}
  else bad "uhd_find_devices found no X410 at $DATA"; fi
  MGMT=${MGMT:-$DATA}
  echo "== preflight: $FATAL fatal, $ISSUES warnings"
  if [ ${#SUDO_CMDS[@]} -gt 0 ]; then echo "== sudo commands for the operator (NOT run by this script):"; printf '   %s\n' "${SUDO_CMDS[@]}"; fi
}

launch_cmd() {
  local freq="--ssb $SSB"; [ "${SCAN:-0}" = 1 ] && freq="--ue-scan-carrier"
  RXARGS="--usrp-args type=x4xx,addr=$DATA,mgmt_addr=$MGMT -O $CFG -r $PRB --numerology 1 --band 78 -C $CARRIER $freq --ue-rxgain $RXG"
  RXARGS="$RXARGS --ue-nb-ant-rx $NANT --ue-nb-ant-tx $NANT --passive-rx --ue-fo-compensation --initial-fo $INITIALFO"
  RXARGS="$RXARGS --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90 ${EXTRA:-}"
}

case "$MODE" in
check) preflight; [ "$FATAL" -eq 0 ] ;;
dry-run|run)
  preflight
  launch_cmd
  ma=$(awk '/MemAvailable/{print $2}' /proc/meminfo); capk=$(( ma - 524288 )); [ $capk -gt 7340032 ] && capk=7340032
  OUT=$OUTBASE/${ARM:-s6ota}_$(date +%Y%m%d_%H%M%S)
  CMD="systemd-run --user --scope --quiet -p MemoryMax=${capk}K -p MemorySwapMax=1G choom -n 1000 -- sudo -n env ${XENV:-} timeout -s INT $DUR $BIN $RXARGS"
  echo "== launch (cwd $OUT, log $OUT/run.log):"; echo "   flock -x /tmp/td_measure.lock $CMD"
  [ "$MODE" = dry-run ] && exit 0
  [ -n "$ARM" ] || { echo "run needs an ARM name"; exit 2; }
  [ "$FATAL" -eq 0 ] || { echo "ABORT: $FATAL fatal preflight items"; exit 3; }
  [ "$ISSUES" -eq 0 ] || [ "${FORCE:-0}" = 1 ] || { echo "ABORT: $ISSUES warnings (fix with the sudo commands above, or FORCE=1)"; exit 3; }
  mkdir -p "$OUT"; cp "$CFG" "$OUT/"; [ -r "$ENVFILE" ] && cp "$ENVFILE" "$OUT/"
  { echo "CMD: $CMD"; echo "HEAD: $(git -C "$R" rev-parse HEAD 2>/dev/null)"; } > "$OUT/cmdline.txt"
  miss0=$(cat /sys/class/net/$NIC/statistics/rx_missed_errors); oob0=$(ethtool -S "$NIC" 2>/dev/null | awk '/rx_out_of_buffer/{print $2}')
  ( exec 9>/tmp/td_measure.lock; flock -x 9
    ma=$(awk '/MemAvailable/{print $2}' /proc/meminfo); [ "$ma" -ge 7000000 ] || { echo "MemAvailable $ma kB < 7 GB at lock time: not started"; exit 3; }
    cd "$OUT" && $CMD > "$OUT/run.log" 2>&1 < /dev/null ) &
  LP=$!
  ( while kill -0 $LP 2>/dev/null; do echo "$(date +%s),$(cat /sys/class/net/$NIC/statistics/rx_missed_errors),$(wc -l < "$OUT/run.log" 2>/dev/null || echo 0)"; sleep 1; done > "$OUT/nic.csv" ) &
  # stall watchdog: a receiver whose log stops growing for 60 s gets SIGINT (twice), never SIGKILL
  ( last=0; still=0; while kill -0 $LP 2>/dev/null; do s=$(stat -c %s "$OUT/run.log" 2>/dev/null || echo 0)
      if [ "$s" = "$last" ]; then still=$((still+5)); else still=0; last=$s; fi
      rp=$(our_rx $LP); if [ $still -ge 60 ] && [ -n "$rp" ]; then echo "$(date +%T) STALL: log silent 60 s -> SIGINT $rp" >> "$OUT/watchdog.txt"; sudo -n kill -INT "$rp"; sleep 30; still=0; fi
      sleep 5; done ) &
  # timeout -s INT ends the run at DUR; if the receiver is still alive 30 s later it gets one more SIGINT; never SIGKILL
  ( sleep $((DUR+30)); rp=$(our_rx $LP); [ -n "$rp" ] && { echo "$(date +%T) alive 30 s after the DUR SIGINT -> second SIGINT $rp" >> "$OUT/watchdog.txt"; sudo -n kill -INT "$rp"; } ) &
  wait $LP; rc=$?
  pgrep -x nr-uesoftmodem >/dev/null && echo "note: an nr-uesoftmodem is still running (ours ended rc=$rc; never SIGKILL an X410 receiver)"
  L=$OUT/run.log
  { echo "rc=$rc nic_miss_delta=$(( $(cat /sys/class/net/$NIC/statistics/rx_missed_errors) - miss0 )) out_of_buffer_delta=$(( $(ethtool -S "$NIC" 2>/dev/null | awk '/rx_out_of_buffer/{print $2}') - ${oob0:-0} ))"
    for p in 'CORE_PLAN' 'HOST_MEM' 'GPU_MEM' 'GPU_PATH' 'Cell Detected' 'Initial sync successful' 'SIB1 decoded' 'ACQ carrier' 'ACQ_STATE' 'PDSCHQ queued' 'SOLVER' 'RFSTALL' 'Out of memory|OOM|Killed'; do
      printf '%-24s %6d  %s\n' "$p" "$(grep -acE "$p" "$L")" "$(grep -aE "$p" "$L" | tail -1 | cut -c1-200)"; done; } | tee "$OUT/summary.txt"
  exit $rc ;;
*) sed -n 2,20p "$0"; exit 2 ;;
esac
