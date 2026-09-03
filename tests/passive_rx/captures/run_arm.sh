#!/bin/bash
# One capture arm, retried until the run is VALID.
#
# WHY THE RETRY LOOP. This receiver mis-locks its CFO at acquisition often enough that a single
# capture is not evidence: when it mis-locks, EVERY stream reads 0.0 % at once -- SIB1, DL PDSCH and
# PUSCH -- so a bad lock is indistinguishable from a real regression unless it is detected and
# thrown away. Four consecutive captures in this session were mis-locks, and one of them was very
# nearly read as "the queue broke the receiver". The verdict below is what makes that impossible.
#
# CFOAPPLY=1 arms the RETUNE. Measurement (ISAC_CFO_TRACK_HZ) is always on and is free; APPLY is
# not, because the correction is delivered by nrue_ru_reinit() -- a full device teardown and
# re-open -- and on this X410 that can hit a stale MPM claim, after which the process dies with
# rpc::timeout on rfdc_set_nco_freq. Measured 2026-08-28: 5/5 runs VOID with APPLY armed against
# roughly 1-in-2 without, and the failing runs showed ANTPOW ~5 (no signal) rather than 90-300.
# APPLY trades a recoverable mis-lock for an unrecoverable dead radio. Default OFF.
# The CFO trim loop (ISAC_CFO_TRACK_*) can rescue a mis-lock, but delivery is a full device re-init
# and re-acquisition, so it needs a run long enough to pay for itself -- hence DUR default 200 s
# rather than the 95 s that was too short to see the benefit.
#
# ARM=<name> CONF=<path> DUR=<s> TRIES=<n> RXG=<dB>
set -u
ARM=${ARM:?set ARM}
CONF=${CONF:?set CONF}
DUR=${DUR:-200}
TRIES=${TRIES:-3}
RXG=${RXG:-40}
NANT=${NANT:-4}
MRC=${MRC:-3}          # PDSCH decoder: MRC over live branches
SENSECOMB=${SENSECOMB:-1}  # sensing grid: co-phased branch combining (STO/SFO/CFO run on this)
# CFO DELIVERY. CONTFO=1 selects upstream OAI's continuous compensation: the PI loop on
# UE->freq_offset is applied DIGITALLY per OFDM symbol in the FEP and the LO is never retuned
# (executables/nr-ue.c:244 skips nrue_ru_set_freq). That is what makes it safe here -- the retune
# path is the one that killed the radio 2/2 with rpc::timeout on rfdc_set_nco_freq.
CONTFO=${CONTFO:-}
FSP=${FSP:-0.05}    # upstream default 0.01 needs ~265 PBCH (5 s) to pull in a 4 kHz mis-lock
FSI=${FSI:-0.001}
MGMT=${MGMT:-128.178.122.174}   # X410 mgmt (was .3 on the retired unit)
DATA=${DATA:-192.168.20.2}      # X410 sfp1
DPDK=${DPDK:-}                  # empty = kernel socket path (no uhd.conf here, so DPDK is unconfigured)
BASE=/home/sens/NICOLA/captures

# ---------------------------------------------------------------------------
# LINK PREFLIGHT. An afternoon of captures was lost 2026-09-01 to a silent
# NetworkManager revert: enp129s0f0np0 went back to 192.168.10.45/24 while the
# X410 sfp1 sits on 192.168.20.2/24, so UHD quietly fell back to the 1 GbE mgmt
# port and starved the stream. Every measurement taken over it read as a code
# regression. The IP is now persistent in the NM profile and the ring is not
# reboot-persistent at all, so assert all of it here rather than trusting either.
# This is the ONE place every runner (ab_*.sh, keep_live.sh) passes through.
NIC=${NIC:-enp129s0f0np0}
preflight() {
  ip -4 addr show "$NIC" | grep -q "192.168.20.1/24" \
    || { echo "  preflight: restoring 192.168.20.1/24 on $NIC"; sudo ip addr add 192.168.20.1/24 dev "$NIC"; }
  [ "$(cat /sys/class/net/$NIC/mtu)" = 9000 ] \
    || { echo "  preflight: restoring MTU 9000"; sudo ip link set "$NIC" mtu 9000; }
  ethtool -g "$NIC" | awk '/Current hardware/,0' | grep -qE '^RX:[[:space:]]+8192' \
    || { echo "  preflight: restoring rx/tx ring 8192"; sudo ethtool -G "$NIC" rx 8192 tx 8192; }
  # BUILD FRESHNESS. 2026-09-02: a 40-minute baseline was captured against a binary
  # that predated the per-illuminator LOS work and the max_pos_acc anti-windup clamp --
  # the tree had both, ran_build/build did not. Same class as the dlopen'd-plugin trap,
  # but for the executable itself: nothing in the run says which code produced it.
  # A stale-source false alarm (a git checkout bumps mtimes) costs one rebuild;
  # a stale binary costs the whole capture and is invisible in the logs.
  REPO=${REPO:-/home/sens/NICOLA/openairinterface5g-total-passive-ue}
  # BIN lets an A/B run a DELIBERATELY older binary as its control arm. Setting it explicitly
  # also waives the freshness check below -- the whole point of that arm is a stale binary --
  # so it is announced loudly rather than passing silently.
  if [ -n "${BIN:-}" ]; then
    echo "  preflight: BIN override -> $BIN (freshness check WAIVED)"
    [ -x "$BIN" ] || { echo "PREFLIGHT ABORT: BIN $BIN not executable"; exit 5; }
  else
  BIN=$REPO/cmake_targets/ran_build/build/nr-uesoftmodem
  newer=$(find "$REPO/openair1" "$REPO/executables" "$REPO/radio" \
            \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.h' \) \
            -newer "$BIN" -print -quit 2>/dev/null)
  [ -z "$newer" ] || { echo "PREFLIGHT ABORT: source newer than binary -- rebuild first ($newer)"; exit 4; }
  fi
  export BIN
  # NIC / SOFTMODEM CPU SEPARATION -- deployed 2026-09-02 after winning its own A/B 4/4 with
  # zero overlap (p=0.029): every "off" run dropped 44k-211k frames, every "on" run dropped
  # exactly zero, and CPI yield and cfotrk activity tightened at the same time. All 14 mlx5
  # completion IRQs default to one-per-core across 0-13, i.e. on top of cpu0 where UEthread_0
  # drains UHD; NET_RX softirq then cannot run when it needs to, the ring overflows even though
  # the box is ~75 % idle, and recv() times out on a stream that IS arriving. Not persistent
  # across reboot, which is exactly why it is asserted here rather than set once by hand.
  # NOSEP=1 disables both halves (for an A/B that needs the stock arm).
  if [ -z "${NOSEP:-}" ]; then
    n=0
    for i in 152 155 156 157 158 159 160 161 162 163 164 165 166 167; do
      [ -e /proc/irq/$i/smp_affinity_list ] || continue
      echo "$(( 8 + n % 6 ))" | sudo tee /proc/irq/$i/smp_affinity_list >/dev/null
      n=$((n+1))
    done
    CPUSET=${CPUSET:-0-7}
    echo "  preflight: NIC irqs -> 8-13 ($n set), softmodem -> cpus $CPUSET"
  fi
  ping -c1 -W2 "$DATA" >/dev/null 2>&1 \
    || { echo "PREFLIGHT ABORT: X410 data plane $DATA unreachable over $NIC"; exit 3; }
  echo "  preflight ok: $NIC 192.168.20.1/24 mtu $(cat /sys/class/net/$NIC/mtu) ring $(ethtool -g $NIC | awk '/Current hardware/,0' | awk '/^RX:/{print $2}') -> $DATA"
}
nic_miss() { awk '{print $1}' /sys/class/net/$NIC/statistics/rx_missed_errors 2>/dev/null || echo 0; }
preflight
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets || exit 1

# CARRIER/SSB: cell moved to dl_arfcn=630000 (3450 MHz) / dl_ssb_arfcn=627264 on 2026-09-03.
# ssb_start_subcarrier = (F_ssb - pointA)/scs - 120, F_ssb=3408.96 MHz, pointA=3400.86 MHz -> 150.
# SCAN=1 drops the hardcoded --ssb and blind-searches instead (--ue-scan-carrier) -- independent
# of any pre-computed SSB position, at the cost of a known crash risk: blind GSCN scan at
# --ue-nb-ant-rx 4 / 273 PRB has previously segfaulted (malloc16, ~1.6 GB of simultaneous
# per-antenna scan buffers). Testing it here on purpose.
CARRIER=${CARRIER:-3450000000}
SCAN=${SCAN:-0}
SSB=${SSB:-150}
if [ "$SCAN" = 1 ]; then FREQARGS="--ue-scan-carrier"; else FREQARGS="--ssb $SSB"; fi

for t in $(seq 1 "$TRIES"); do
  OUT=$BASE/${ARM}_$(date +%H%M%S); mkdir -p "$OUT"
  # RELEASE THE X410 CLAIM GRACEFULLY BEFORE PROBING. Root-caused 2026-09-02 from the X410's own
  # MPM journal, which is the only place it is visible:
  #   11:44:02 [MPM.RPCServer] [WARNING] Someone tried to claim this device again (From: <this host>)
  #   11:44:02 [MPM.kill] [INFO] Terminating pid: 141048
  #   11:44:02 systemd[1]: Stopping USRP Hardware Daemon (MPM)...
  #   11:45:46 systemd[1]: Started USRP Hardware Daemon (MPM).
  # A SECOND CLAIM MAKES MPM KILL ITSELF, and it is down for ~100 s. SIGKILL never lets UHD
  # release the claim, so the claim goes stale, and the uhd_usrp_probe below -- our own liveness
  # check -- is then the second claimant that shoots the daemon. The run launched into that
  # window sees returned=0 / ERROR_CODE_TIMEOUT for its whole life with healthy ANTPOW and ZERO
  # NIC drops, i.e. exactly the "X410 stream stall" signature. By the time anything re-probes,
  # MPM is back and the probe SUCCEEDS, which is why the cause stayed hidden.
  # SIGTERM first, wait for the process to actually go, SIGKILL only as a fallback.
  sudo pkill -TERM -x nr-uesoftmodem 2>/dev/null
  for _w in $(seq 1 20); do pgrep -x nr-uesoftmodem >/dev/null || break; sleep 1; done
  if pgrep -x nr-uesoftmodem >/dev/null; then
    echo "  (softmodem ignored SIGTERM -- falling back to SIGKILL; expect a stale MPM claim)"
    sudo pkill -9 -x nr-uesoftmodem 2>/dev/null
    sleep 3
  fi
  sleep 2
  # RESTART usrp-hwd ONLY IF THE DEVICE IS ACTUALLY UNREACHABLE. It used to run unconditionally
  # before every try, costing 40 s of blackout each time AND churning a device that is documented to
  # need settling afterwards (ANTPOW sits at the noise floor for a while after an MPM restart). The
  # restart exists for a STALE MPM CLAIM, so test for that instead of assuming it. Override with
  # FORCE_HWD=1.
  sudo rm -rf /var/run/dpdk/* /dev/hugepages/* 2>/dev/null
  if [ -n "${FORCE_HWD:-}" ] || ! timeout 30 uhd_usrp_probe --args "type=x4xx,addr=$DATA,mgmt_addr=$MGMT" 2>&1 \
       | grep -qE "X410|Device: X400"; then
    echo "  (X410 unreachable or FORCE_HWD set -- restarting usrp-hwd)"
    ssh -o BatchMode=yes -o StrictHostKeyChecking=no root@$MGMT "systemctl restart usrp-hwd" >/dev/null 2>&1
    sudo rm -rf /var/run/dpdk/* /dev/hugepages/* 2>/dev/null
    sleep 40
  fi
  for a in 1 2 3 4; do
    timeout 45 uhd_usrp_probe --args "type=x4xx,addr=$DATA,mgmt_addr=$MGMT" 2>&1 \
      | grep -qE "X410|Device: X400" && break
    sleep 12
  done
  sudo rm -rf /var/run/dpdk/* /dev/hugepages/* 2>/dev/null; sleep 2

  B=$(ssh sens4 "stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log")
  MISS0=$(nic_miss)
  # PER-SLOT DIAGNOSTICS ON BY DEFAULT. Set DIAGOFF=1 to run without them.
  #
  # This block used to carry an n=1 claim that turning them OFF "MADE THINGS WORSE" (runs of
  # 10-30 s against 127 s). RETRACTED 2026-09-02: that pair was captured while the host had
  # silently reverted to its 1 GbE management address, so UHD was starved and EVERY measurement
  # from that window read as a code effect. The preflight above now makes that specific failure
  # impossible, but the conclusion drawn under it does not survive. Their real cost is also now
  # measured and small -- the 683 lines/s figure was dominated by the TSYNC_PDCCH flood, which is
  # gated separately on ISAC_TSYNC_AUDIT -- and log rate turns out to track receiver HEALTH rather
  # than harm it (the 2195 lines/s run had zero NIC drops; the 41 lines/s run had 45k).
  sudo env ISAC_DISC_NO_RESYNC=1  \
    ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1 ISAC_PUSCH_DIAG=1 \
   ${PDCCHTIMING:+ISAC_PDCCH_TIMING=1} ${PUSCHTIMING:+ISAC_PUSCH_TIMING=1} ${PUSCHDIAG:+ISAC_PUSCH_DIAG=1} \
    ISAC_CFO_TRACK_HZ=800 ISAC_CFO_TRACK_PERIOD=20 ${CFOAPPLY:+ISAC_CFO_TRACK_APPLY=1} \
    ISAC_UL_TA_SWEEP=${TASWEEP:-0:0:0} ${ULPROBE:+ISAC_UL_PROBE=1} \
    ${GAINTRIM:+ISAC_RX_GAIN_TRIM=$GAINTRIM} \
    ${MRC:+ISAC_RX_MRC_MODE=$MRC} ${BRMIN:+ISAC_RX_BRANCH_MIN_DB=$BRMIN} ${RXBRANCH:+ISAC_RX_BRANCH=$RXBRANCH} ${NVARFIX:+ISAC_RX_NVAR_FIX=$NVARFIX} ${BRFO:+ISAC_RX_BRANCH_FO=$BRFO} \
    ${SENSECOMB:+ISAC_SENSE_COMB=$SENSECOMB} ${SLOTPOOL:+ISAC_SLOT_POOL=$SLOTPOOL} \
    ${SYNCONLY:+ISAC_SYNC_ONLY=$SYNCONLY} \
    ${TSYNCAUDIT:+ISAC_TSYNC_AUDIT=$TSYNCAUDIT} \
    ISAC_TSYNC_RESET=${TSYNCRESET:-0} \
    ${CPUSET:+CPUSET=$CPUSET} BIN=$BIN \
    setsid nohup bash -c "ulimit -c 0; exec timeout $DUR ${CPUSET:+taskset -c $CPUSET} \
    $BIN \
    --usrp-args type=x4xx,addr=$DATA,mgmt_addr=$MGMT${DPDK:+,use_dpdk=$DPDK} \
    -O $CONF -r 273 --numerology 1 --band 78 -C $CARRIER $FREQARGS --ue-rxgain $RXG \
    --ue-nb-ant-rx $NANT --ue-nb-ant-tx $NANT --passive-rx --ue-fo-compensation \
    ${CONTFO:+--cont-fo-comp $CONTFO --freq-sync-P $FSP --freq-sync-I $FSI} \
    ${OFFDIV:+--offset-divisor $OFFDIV} \
    --thread-pool 0,1,4,5,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
    > "$OUT/run.log" 2>&1 < /dev/null &
  sleep 5
  # NIC DROP TIME SERIES. 2026-09-02: stalls and rx_out_of_buffer correlate across runs
  # (Spearman ~0.9, n=5), but the ARROW IS UNKNOWN -- an RFSTALL triggers a full device
  # re-init, during which nothing drains the ring, so the drops could just as easily be
  # the CONSEQUENCE of a stall as its cause (22k-45k drops per stall, suspiciously
  # consistent). Same trap as the -6.6 dB correlation, which turned out to be backwards.
  # This resolves it: 1 Hz samples of the counters plus run.log's line count, so an
  # RFSTALL's line number maps to a wall-clock time and can be placed before or after
  # the drop burst. Passive -- reads sysfs, changes nothing.
  ( while pgrep -x nr-uesoftmodem >/dev/null; do
      echo "$(date +%s),$(cat /sys/class/net/$NIC/statistics/rx_missed_errors),$(cat /sys/class/net/$NIC/statistics/rx_packets),$(wc -l < "$OUT/run.log" 2>/dev/null || echo 0)"
      sleep 1
    done > "$OUT/nic.csv" ) &
  # CFO MIS-LOCK WATCHDOG. The trim loop's own gate (streak >= 5 agreeing windows, spread < 500 Hz,
  # |ema| > thr) is what CFOAPPLY used to fire on. It discriminates correctly: it said stable=yes on
  # both measured mis-locks and withheld on a noisy-but-recoverable lock. But ACTING on it means
  # nrue_ru_reinit(), which killed the radio in 2 of 2 runs (once rpc::timeout on rfdc_set_nco_freq,
  # once left deaf at the noise floor). Retrying costs 4 minutes, so abort instead of retuning.
  ( while pgrep -x nr-uesoftmodem >/dev/null; do
      if [ -z "$CONTFO" ] && grep -aq "CFOTRK .*stable=yes" "$OUT/run.log" 2>/dev/null; then
        touch "$OUT/cfo_mislock"; sudo pkill -9 -x nr-uesoftmodem; break
      fi
      sleep 5
    done ) &
  WATCH=$!
  while pgrep -x nr-uesoftmodem >/dev/null; do sleep 5; done
  kill $WATCH 2>/dev/null
  A=$(ssh sens4 "stat -c%s /home/sens/NICOLA/gnbLogs/gnb.log")
  echo "gnb_log_bracket $B $A" > "$OUT/bracket.txt"

  L=$OUT/run.log
  SIB=$(grep -ac 'SIB1 decoded' "$L")
  NACK=$(grep -ac 'Got NACK on NR-BCCH' "$L")
  CPI=$(grep -ac 'SENSING: CPI #' "$L")
  DLOK=$(grep -aoE 'LDPCDIAG ok=[0-9]+' "$L" | tail -1 | cut -d= -f2)
  # DL health by SEGMENTS, not by 'ok='. That counter includes the gNB's all-zero TBs (30-45% here),
  # so a CFO mis-lock reads ok=141316 while only 3.5% of segments actually decode.
  # TB DECODE RATE = ok/(ok+seg_fail). NOT segs_decoded: that pair of counters is summed ONLY over
  # FAILING TBs (nr_pdsch_passive_decode.c:302-303,567-569), so it measures how NEAR-MISS the
  # failures were and DROPS as decoding improves. Scoring on it voided healthy runs all morning.
  SEGP=$(grep -aoE 'LDPCDIAG ok=[0-9]+ seg_fail=[0-9]+' "$L" | tail -1 \
         | awk -F'[= ]' '{ t=$3+$5; if (t>0) printf "%d", 100*$3/t; else print 0 }')
  CLAIM=$(grep -aoE 'claimed=[0-9]+' "$L" | tail -1)
  CFOT=$(grep -ac CFOTRK "$L")
  if   [ -f "$OUT/cfo_mislock" ];   then V=VOID_CFO_MISLOCK
  elif [ "$SIB" -eq 0 ];            then V=VOID_NO_SIB1
  elif [ "${DLOK:-0}" -eq 0 ];      then V=VOID_DL_ZERO
  elif [ "$CPI" -eq 0 ];            then V=VOID_NO_CPI
  elif [ "${SEGP:-0}" -lt 15 ];     then V=VOID_DL_RATE
  else V=VALID; fi
  echo "arm=$ARM try=$t/$TRIES verdict=$V sib1=$SIB nack=$NACK dl_ldpc_ok=${DLOK:-0} dl_tb=${SEGP:-0}% cpis=$CPI $CLAIM cfotrk=$CFOT nic_miss=$(( $(nic_miss) - MISS0 ))" \
    | tee "$OUT/verdict.txt"
  if [ "$V" = VALID ]; then echo "$OUT" > $BASE/${ARM}_LAST_VALID; break; fi
done
echo "=== ARM $ARM DONE ==="
