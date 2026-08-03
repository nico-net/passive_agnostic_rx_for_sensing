#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-CSSL-1.0
#
# Test of the --passive-rx receive-only mode (TOTAL_PASSIVE_UE_HANDOVER.md Phase 1/2), covering
# Stage 1 (single receiver), Stage 2 (two receivers + iperf + fusion), and the N-receiver
# no-AoA comparison case -- /home/sens/.claude/plans/zesty-baking-thompson.md,
# PHASE3_BLIND_PDCCH_LIVE_WIRING_HANDOVER.md:
#
#   nr-softmodem   SA gNB, rfsim server, attached to a local open5gs core
#   nr-uesoftmodem ACTIVE UE 1..M -- attach normally (RA -> RRC -> NAS -> PDU session), each carries
#                                     its own iperf3 stream, each a distinct open5gs subscriber
#                                     (ue.active.conf, ue.active2.conf, ue.active3.conf)
#   nr-uesoftmodem PASSIVE UE 1..N -- --passive-rx, never transmits, each a distinct surveyed
#                                      position (ue.passive.conf, ue.passive2.conf, ue.passive3.conf)
#
# Why this works: the rfsimulator server writes the SAME downlink sample block to every connected
# client socket (proven for up to 3 clients; MAX_FD_RFSIMU=250 means more is no different), so
# every passive receiver sees bit-for-bit the downlink the active UE receives.
#
# Prerequisites (checked below):
#   - open5gs running locally, AMF NGAP on 127.0.0.1:38412
#   - the subscriber in ue.active.conf provisioned in the open5gs mongodb
#   - passwordless sudo (the active UE needs CAP_NET_ADMIN for its TUN; the passive UEs do not)
#   - iperf3 installed (sustained DL traffic generator, alongside ping)
#   - repos/isac's isac-track built in --release (fusion step; skipped with a warning if absent)
#
# Usage: ./run_passive_rx.sh [duration_s] [out_dir]
# Env vars: NUM_RX=2|3 (default 2, how many passive receivers to run)
#           NUM_UE=1|2|3 (default 3, how many independently-attached active UEs to generate traffic
#                     -- each gets its own RNTI/iperf3 stream, so the blind-PDCCH source sees more
#                     than one RNTI in flight, closer to a real multi-UE cell than a single stream)
#           IPERF_RATE (default 3M, iperf3 -b argument PER UE -- raise this for denser DL traffic/CFR
#                     data; total DL load scales with NUM_UE * IPERF_RATE)
#           RX1_NANT/RX2_NANT/RX3_NANT (default 4 each, --ue-nb-ant-rx per receiver -- every
#                     receiver's .conf now carries its own rx_array/aoa_broadside_deg, oriented for
#                     that receiver's own geometry; see ue.passive{,2,3}.conf's rx_array comments.
#                     Set to 1 to run a given receiver single-antenna/no-AoA instead, e.g. to model a
#                     heterogeneous fleet with a B210-class single-channel receiver.)
#           BW100 (default 0, set to 1 to switch to the 100MHz/273PRB cell -- gnb.sa.rfsim.100mhz.conf
#                  + ue.passive*.100mhz.conf, see the latter's header for what's live-verified vs
#                  predicted-not-yet-reverified at this bandwidth)
set -u

DURATION="${1:-90}"
OUT_DIR="${2:-/tmp/passive_rx}"
NUM_RX="${NUM_RX:-2}"   # 1 is supported: see the birth-flag note in the FUSION section
NUM_UE="${NUM_UE:-3}"
IPERF_RATE="${IPERF_RATE:-3M}"
TRAFFIC="${TRAFFIC:-udp}"   # udp = udp_dl.py (1% CPU/stream) | iperf3 (100% CPU/stream, measured)
# CONF_TAG selects a receiver-conf variant: "" = base, ".aoa" = the 2-receiver AoA scene
# (ue.passive{,2}.aoa.100mhz.conf), ".upa" = the 2x2 square arrays (README.upa.md).
CONF_TAG="${CONF_TAG:-}"
# PIN defaults OFF: taskset-based whole-process pinning CRASHES this OAI build. Measured 2026-07-29 --
# with `taskset -c` applied, a netns'd active UE aborts during startup at system.c:273's
# AssertFatal on pthread_setname_np() returning ENOENT (the named thread's /proc/self/task/<tid>
# entry is already gone, i.e. it exited before being named -- a latent startup race that a
# constrained CPU set makes very likely). PIN=0 attaches all 3 UEs reliably; PIN=1 does not.
# Left in place because the pinning idea is sound and the fix belongs upstream, not here.
PIN="${PIN:-0}"
RX1_NANT="${RX1_NANT:-4}"
RX2_NANT="${RX2_NANT:-4}"
RX3_NANT="${RX3_NANT:-4}"
BW100="${BW100:-0}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/../../cmake_targets/ran_build/build"
ISAC_TRACK="/home/sens/NICOLA/repos/isac/target/release/isac-track"

# All 3 confs' out_path/report_path are hardcoded absolute paths (see each .conf's header comment),
# NOT $OUT_DIR-relative -- kept as-is from Stage 1/2 rather than silently diverging from what the
# .conf files actually say.
if [ "$BW100" -eq 1 ]; then
  GNB_CONF="gnb.sa.rfsim.100mhz.conf"
  ALL_RX_CONFS=("ue.passive${CONF_TAG}.100mhz.conf" "ue.passive2${CONF_TAG}.100mhz.conf" "ue.passive3${CONF_TAG}.100mhz.conf")
  ALL_RX_REPORTS=("/tmp/passive_rx/passive_reports_100m.jsonl" "/tmp/passive_rx/passive_reports_rx2_100m.jsonl" "/tmp/passive_rx/passive_reports_rx3_100m.jsonl")
  # --ssb sets ssb_start_subcarrier (nr-uesoftmodem.h), NOT an SSB ARFCN -- default 516, left
  # --ssb is ssb_start_subcarrier (nr-uesoftmodem.h), NOT an SSB ARFCN -- and it is BANDWIDTH- AND
  # PLACEMENT-DEPENDENT, which is the root cause of the "273 PRB won't sync" problem first seen here.
  # 516 (the 106-PRB value) is simply WRONG for this cell. The correct value follows directly from
  # nr_common.c's get_ssb_first_sc(): (ssbCentreFreq - pointA)/scs - 120, where
  # pointA = fc - (nbRB/2)*scs*12 = 3750.00 - 49.14 = 3700.86 MHz and the SSB (ARFCN 649920) sits at
  # 3748.80 MHz -> (3748.80-3700.86)e6/30e3 - 120 = 1478. Cross-checked against the UE's own blind
  # scan, which independently reports "GSCN: 8019, with SSB offset: 1478, SSB Freq: 3748800000".
  #
  # Using the correct value here (rather than the earlier --ue-scan-carrier workaround) ALSO fixes a
  # hard crash and speeds the run up substantially -- see the handover doc's 100MHz section:
  # --ue-scan-carrier at 273 PRB with --ue-nb-ant-rx 4 SEGFAULTS in nr_initial_sync() (unchecked
  # malloc16 at nr_initial_sync.c:403; ~40 GSCN candidates x 4 antennas x ~9.8MB/antenna of scan
  # buffer is ~1.6GB, the allocation fails and the following memcpy dereferences NULL), and even
  # when it does not crash the blind scan wastes tens of seconds of every run's sensing window.
  CELL_ARGS=(-C 3750000000 -r 273 --numerology 1 --band 78 --ssb 1478)
else
  GNB_CONF="gnb.sa.rfsim.conf"
  ALL_RX_CONFS=("ue.passive${CONF_TAG}.conf" "ue.passive2${CONF_TAG}.conf" "ue.passive3${CONF_TAG}.conf")
  ALL_RX_REPORTS=("/tmp/passive_rx/passive_reports.jsonl" "/tmp/passive_rx/passive_reports_rx2.jsonl" "/tmp/passive_rx/passive_reports_rx3.jsonl")
  # Cell parameters -- must match gnb.sa.rfsim.conf (absoluteFrequencySSB 621312 -> 3319.68 MHz).
  CELL_ARGS=(-C 3319680000 -r 106 --numerology 1 --ssb 516 --band 78)
fi
ALL_RX_LABELS=("rx1" "rx2" "rx3")
RX_CONFS=("${ALL_RX_CONFS[@]:0:$NUM_RX}")
RX_REPORTS=("${ALL_RX_REPORTS[@]:0:$NUM_RX}")
RX_LABELS=("${ALL_RX_LABELS[@]:0:$NUM_RX}")

# Prefer each conf's OWN report_path over the hardcoded ALL_RX_REPORTS above. Those arrays only ever
# knew about the base and .100mhz variants, so any new CONF_TAG whose confs write elsewhere would
# have been SCORED AGAINST ANOTHER ARM'S FILE -- silently, and looking entirely plausible. Added
# 2026-07-30 while building the pdsch_data A/B, whose control arm needs distinct report paths
# precisely so the two arms cannot contaminate each other. Falls back to the array when a conf has
# no report_path line, so existing behaviour is unchanged.
for i in "${!RX_CONFS[@]}"; do
  conf_report=$(sed -n 's/^[[:space:]]*report_path[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
                "$SCRIPT_DIR/${RX_CONFS[$i]}" | head -1)
  if [ -n "$conf_report" ] && [ "$conf_report" != "${RX_REPORTS[$i]}" ]; then
    echo "note: ${RX_LABELS[$i]} report path taken from ${RX_CONFS[$i]}: $conf_report"
    RX_REPORTS[$i]="$conf_report"
  fi
done

ALL_UE_CONFS=("ue.active.conf" "ue.active2.conf" "ue.active3.conf")
ALL_UE_LABELS=("ue1" "ue2" "ue3")
UE_CONFS=("${ALL_UE_CONFS[@]:0:$NUM_UE}")
UE_LABELS=("${ALL_UE_LABELS[@]:0:$NUM_UE}")

for c in "${RX_CONFS[@]}"; do
  [ -f "$SCRIPT_DIR/$c" ] || { echo "ERROR: receiver conf $c not found (CONF_TAG='$CONF_TAG')" >&2; exit 1; }
done

GNB_LOG="$OUT_DIR/gnb.log"
UE_ACTIVE_LOGS=()
for label in "${UE_LABELS[@]}"; do UE_ACTIVE_LOGS+=("$OUT_DIR/${label}_active.log"); done
RX_LOGS=()
for label in "${RX_LABELS[@]}"; do RX_LOGS+=("$OUT_DIR/ue_${label}.log"); done

mkdir -p "$OUT_DIR"
# The .conf files' out_path/report_path are hardcoded to /tmp/passive_rx/... (see the "Hardcoded,
# not $OUT_DIR-relative" note above) -- ensure that directory exists regardless of $OUT_DIR. This
# was previously masked by residual state from earlier runs; found live when /tmp/passive_rx got
# cleaned (systemd-tmpfiles or similar) mid-session and every receiver silently produced 0 reports.
mkdir -p /tmp/passive_rx
# sensing_engine.cc opens report_path/out_path files in APPEND mode (std::ios::app, by design -- a
# single long-running receiver process writes one line per completed CPI) -- meaning across SEPARATE
# invocations of this script at the same hardcoded /tmp/passive_rx/* paths, reports silently
# ACCUMULATE forever unless cleared here. Found live 2026-07-28: a fusion run's tracks scored
# ~612s older than the run's own ground-truth window, i.e. isac-track was replaying leftover reports
# from an EARLIER run this session, not just the current one. Clear before every run.
rm -f /tmp/passive_rx/passive_reports*.jsonl /tmp/passive_rx/passive_sensing*

# Both UEs default to Mod_id=0 and each writes nrL1_UE_stats-0.log (among other nr*_stats.log
# files) into the CWD. The active UE runs under sudo, so its copy is root-owned; when the passive
# UE (non-root) starts afterward and tries to open/truncate the same filename, open() fails and
# nrL1_UE_stats_thread() hits an AssertFatal (found live 2026-07-28 -- looked like a startup crash
# in the passive UE, was actually this stale-file collision from a PRIOR run's active UE). Clean
# up before every run rather than leave it as a one-off manual fix.
sudo -n rm -f "$SCRIPT_DIR"/nr*_stats*.log 2>/dev/null

# UE2/UE3's own netns + veth pair (see ue.active2.conf/ue.active3.conf's header comment for WHY --
# nr-uesoftmodem's PDU session TUN name has no per-process instance knob, so concurrent active UEs
# must be network-namespace-isolated to each get their own "oaitun_ue1"). Mirrors the netns/veth
# pattern tests/sensing_sim's run_sim_traffic_iperf_ns3.sh already uses for extra receivers, and
# OAI's own ci-scripts/yaml_files/5g_vrtsim_multiue (one container = one netns per UE) -- confirmed
# from radio/rfsimulator/simulator.cpp that the rfsim SERVER binds the wildcard address
# (getaddrinfo(NULL,...)+AI_PASSIVE), so it accepts the veth peer connection with no gNB-side change.
UE_NETNS_BY_IDX=("" "oai_pue2" "oai_pue3")   # index 0 (ue1) stays in the default netns
UE_VETH_HOST_BY_IDX=("" "veth-pue2h" "veth-pue3h")
UE_VETH_NS_BY_IDX=("" "veth-pue2n" "veth-pue3n")
UE_HOST_IP_BY_IDX=("" "10.91.2.1" "10.91.3.1")   # MUST match ue.activeN.conf's rfsimulator.serveraddr
UE_NS_IP_BY_IDX=("" "10.91.2.2" "10.91.3.2")

setup_ue_netns() { # setup_ue_netns <idx>
  # NOTE: idx must be its own `local` statement -- `local idx=$1 ns=${ARR[$idx]}` evaluates the
  # SECOND assignment's `$idx` against the value from BEFORE this command ran (unset here), not the
  # one just assigned in the same `local` line (found live: it silently made ns/vh/vn empty strings,
  # so `ip netns add ""` ran instead of `ip netns add oai_pue2`, and the later `ip netns exec
  # "oai_pue2"` -- which correctly indexes the array itself, outside this function -- failed with
  # "Cannot open network namespace" because that name was never actually created).
  local idx="$1"
  local ns="${UE_NETNS_BY_IDX[$idx]}" vh="${UE_VETH_HOST_BY_IDX[$idx]}" vn="${UE_VETH_NS_BY_IDX[$idx]}"
  local hip="${UE_HOST_IP_BY_IDX[$idx]}" nip="${UE_NS_IP_BY_IDX[$idx]}"
  sudo -n ip netns add "$ns"
  sudo -n ip link add "$vh" type veth peer name "$vn"
  sudo -n ip link set "$vn" netns "$ns"
  sudo -n ip addr add "$hip/24" dev "$vh"
  sudo -n ip link set "$vh" up
  sudo -n ip netns exec "$ns" ip addr add "$nip/24" dev "$vn"
  sudo -n ip netns exec "$ns" ip link set "$vn" up
  sudo -n ip netns exec "$ns" ip link set lo up
}

teardown_ue_netns() { # teardown_ue_netns <idx>
  local idx="$1"
  local ns="${UE_NETNS_BY_IDX[$idx]}" vh="${UE_VETH_HOST_BY_IDX[$idx]}"
  sudo -n ip netns pids "$ns" 2>/dev/null | xargs -r sudo -n kill -9 2>/dev/null
  sudo -n ip netns del "$ns" 2>/dev/null
  sudo -n ip link del "$vh" 2>/dev/null   # deleting the ns side via netns-del already removes the peer, but
                                          # be defensive if setup partially failed and the ns side is gone
}

cleanup() {
  echo "--- tearing down ---"
  # Match on the config paths so we never touch unrelated OAI processes on this machine
  # (e.g. a concurrent tests/sensing_sim run). Prefix match "ue.passive"/"ue.active" (not the exact
  # "ue.passive.conf" string) so this catches ue.passive2.conf/ue.active2.conf etc too.
  sudo -n pkill -9 -f "passive_rx/ue.active"        2>/dev/null
  pkill      -9 -f "passive_rx/ue.passive"          2>/dev/null
  pkill      -9 -f "passive_rx/gnb.sa.rfsim"        2>/dev/null # prefix match: catches the 100MHz conf too
  pkill      -9 -f "iperf3 -s -B"                   2>/dev/null
  # iperf3 clients don't reliably self-terminate at their own -t deadline under heavy congestion
  # (found live with NUM_UE=3: 2 clients still running >25s past their "-t 100" duration, each
  # pegging a full core) -- kill explicitly rather than trust -t.
  pkill      -9 -f "iperf3 -c"                      2>/dev/null
  pkill      -9 -f "udp_dl.py"                      2>/dev/null
  for idx in 1 2; do teardown_ue_netns "$idx"; done
  sleep 1
}
trap cleanup EXIT INT TERM

# Namespaces/veths from a prior run that crashed before cleanup ran would collide with setup_ue_netns
# below ("File exists") -- clear defensively before starting, same rationale as the report-file/
# stats-file cleanup above.
for idx in 1 2; do teardown_ue_netns "$idx"; done 2>/dev/null

# --- prerequisites -----------------------------------------------------------------------------
for exe in nr-softmodem nr-uesoftmodem; do
  if [ ! -x "$BUILD_DIR/$exe" ]; then
    echo "ERROR: $BUILD_DIR/$exe not found. Build with:" >&2
    echo "  cmake --build $BUILD_DIR --target nr-softmodem nr-uesoftmodem -j\$(nproc)" >&2
    exit 1
  fi
done
if ! ss -lS 2>/dev/null | grep -q '127.0.0.1:38412'; then
  echo "ERROR: no AMF listening on 127.0.0.1:38412. Start open5gs first." >&2
  exit 1
fi
if ! sudo -n true 2>/dev/null; then
  echo "ERROR: passwordless sudo required (the active UE creates a TUN interface)." >&2
  exit 1
fi

# CPU affinity. The rfsimulator federation is a hard real-time-ish pipeline: every process must keep
# up with the shared sample clock, so one starved process throttles ALL of them (measured: the whole
# federation reaching only ~3% of real time at 273 PRB). Left unpinned, 6+ softmodems each spawning
# an 8-thread pool oversubscribe 12 cores several times over and the scheduler migrates hot threads
# between cores, trashing cache. Pinning to DISJOINT sets sized by each process's real cost --
# gNB heaviest, then the 4-antenna passive receivers, then the 1-antenna active UEs whose only job
# is to keep a PDU session up so the gNB keeps scheduling -- removes both problems.
# Emits nothing when PIN=0, so the previous unpinned behaviour is one env var away.
pin_for() { # pin_for <role> <index> -> echoes a taskset prefix, or nothing
  [ "$PIN" -eq 1 ] || return 0
  local ncpu; ncpu=$(nproc)
  [ "$ncpu" -ge 12 ] || return 0   # the split below assumes >=12 cores; below that, don't pin
  case "$1" in
    gnb)     echo "taskset -c 0-3" ;;
    rx)      case "$2" in
               0) echo "taskset -c 4-6"  ;;
               1) echo "taskset -c 7-9"  ;;
               *) echo "taskset -c 10-11" ;;
             esac ;;
    ue)      echo "taskset -c 10,11" ;;   # all active UEs share: they are the cheapest processes
  esac
}

wait_for() { # wait_for <file> <pattern> <timeout_s> <label>
  local f="$1" pat="$2" t="$3" label="$4" i=0
  while [ "$i" -lt "$t" ]; do
    grep -q "$pat" "$f" 2>/dev/null && { echo "  [ok] $label"; return 0; }
    sleep 1; i=$((i+1))
  done
  echo "  [TIMEOUT] $label (waited ${t}s) -- see $f" >&2
  return 1
}

# --- 1. gNB ------------------------------------------------------------------------------------
echo "--- starting gNB (SA, rfsim server, open5gs) ---"
$(pin_for gnb) "$BUILD_DIR/nr-softmodem" -O "$SCRIPT_DIR/$GNB_CONF" --rfsim >"$GNB_LOG" 2>&1 &
wait_for "$GNB_LOG" "Received NGSetupResponse" 30 "gNB associated with AMF" || exit 1
# NGAP comes up BEFORE the RU/L1, so the rfsim server socket is not necessarily listening yet.
# Starting a UE before it is bound leaves that UE stuck in its connect-retry loop.
i=0
until ss -lnt 2>/dev/null | grep -q ':4043 '; do
  i=$((i+1)); [ "$i" -gt 30 ] && { echo "  [TIMEOUT] rfsim server never listened on :4043" >&2; exit 1; }
  sleep 1
done
echo "  [ok] rfsim server listening on :4043"

# --- 2. active UE(s) ---------------------------------------------------------------------------
echo "--- starting $NUM_UE ACTIVE UE(s) (attach, carry traffic) ---"
UE_IPS=()
for i in "${!UE_CONFS[@]}"; do
  label="${UE_LABELS[$i]}"; conf="${UE_CONFS[$i]}"; log="${UE_ACTIVE_LOGS[$i]}"
  if [ "$i" -eq 0 ]; then
    echo "  $label: default netns"
    sudo -n $(pin_for ue "$i") "$BUILD_DIR/nr-uesoftmodem" -O "$SCRIPT_DIR/$conf" --rfsim \
      "${CELL_ARGS[@]}" >"$log" 2>&1 &
  else
    echo "  $label: own netns (${UE_NETNS_BY_IDX[$i]}, veth to gNB at ${UE_HOST_IP_BY_IDX[$i]})"
    setup_ue_netns "$i"
    sudo -n ip netns exec "${UE_NETNS_BY_IDX[$i]}" $(pin_for ue "$i") "$BUILD_DIR/nr-uesoftmodem" -O "$SCRIPT_DIR/$conf" --rfsim \
      "${CELL_ARGS[@]}" >"$log" 2>&1 &
  fi
  # 150s for the same reason as the passive receivers below: at 273 PRB with several softmodems
  # already running, initial sync + RA legitimately takes longer than the original 60s. Measured
  # timing out (val9) on ue1 with a perfectly healthy UE that had simply not finished syncing.
  wait_for "$log" "RA procedure succeeded"            150 "active UE $label completed random access" || exit 1
  wait_for "$log" "PDU Session Establishment Accept"  150 "active UE $label got a PDU session"       || exit 1
  ip=$(grep -oP 'UE IPv4: \K[0-9.]+' "$log" | tail -1)
  UE_IPS+=("${ip:-}")
  echo "  $label IP: ${ip:-<unknown>}"
done

# All active UEs default to Mod_id=0 (nrL1_UE_stats_thread(), nr-ue.c:118 -- one UE per process, no
# CLI knob for a distinct instance id), so they and the passive UEs all race to open the SAME
# "nrL1_UE_stats-0.log" filename in this CWD regardless of netns (network namespaces don't isolate
# the filesystem). The first (sudo) UE creates it root-owned; open() checks permission bits not
# ownership, so chmod'ing it world-writable here lets everyone else open/truncate it too instead of
# hitting nr-ue.c:121's AssertFatal on startup.
sudo -n chmod 666 "$SCRIPT_DIR"/nr*_stats*.log 2>/dev/null

# --- 3. passive receivers ---------------------------------------------------------------------
# Started AFTER the active UE is connected, so the CSI-RS/PDCCH they sense is already on the air.
# rx2/rx3 are additional surveyed positions for fusion -- see ue.passive2.conf/ue.passive3.conf's
# header comments for what differs per receiver (rx_pos_x/y, distinct rx_id/out_path).
echo "--- starting $NUM_RX PASSIVE UE(s) (--passive-rx) ---"
ALL_RX_NANT=("$RX1_NANT" "$RX2_NANT" "$RX3_NANT")
for i in "${!RX_CONFS[@]}"; do
  label="${RX_LABELS[$i]}"; conf="${RX_CONFS[$i]}"; log="${RX_LOGS[$i]}"; nant="${ALL_RX_NANT[$i]}"
  ANT_ARGS=()
  if [ "$nant" -gt 1 ]; then
    ANT_ARGS=(--ue-nb-ant-rx "$nant")
    echo "  $label: AoA enabled, --ue-nb-ant-rx $nant"
  fi
  $(pin_for rx "$i") "$BUILD_DIR/nr-uesoftmodem" -O "$SCRIPT_DIR/$conf" --rfsim --passive-rx \
    "${CELL_ARGS[@]}" "${ANT_ARGS[@]}" >"$log" 2>&1 &
  # 150s, not 60: the passive receivers start LAST, with the gNB and every active UE already
  # loading the box, and at 273 PRB with 4 antennas initial sync is slow. 60s was marginal and was
  # measured timing out (val8) even though the receiver was healthy -- it had printed its sensing
  # channel setup and started its engine thread, it simply had not reached SIB1 yet.
  wait_for "$log" "SIB1 decoded" 150 "passive UE $label synced and decoded SIB1" || exit 1
done

# --- 4. downlink traffic -----------------------------------------------------------------------
# Real DL PDSCH to EACH active UE, routed host -> ogstun -> UPF -> GTP-U -> gNB -> rfsim -> UE.
# ping alone (Stage 1) gives sparse, bursty grants; a sustained UDP stream keeps the blind-PDCCH
# source (which fires per scheduled slot, not on a fixed period like csirs_monitor) decoding
# continuously. NUM_UE independent streams mean the RNTI-persistence gate sees more than one
# legitimate RNTI in flight.
#
# TRAFFIC=udp (default) uses udp_dl.py instead of iperf3, and it matters a lot here:
# iperf3 was MEASURED at 100-102% CPU for a 6 Mbit/s UDP stream regardless of --pacing-timer
# (1000/20000/50000 us) or TCP-vs-UDP -- it busy-spins. Three streams were burning three of this
# box's twelve cores while the rfsimulator federation, the actual bottleneck, was reaching only
# ~3% of real time. udp_dl.py paces with an absolute-deadline sleep and measures 1.0% CPU (sender)
# + 0.4% (receiver) for the identical 6.00 Mbit/s offered load. Set TRAFFIC=iperf3 to go back.
# Secondary benefit worth knowing: udp_dl.py is CONSTANT-rate, where iperf3 bursts -- steadier DL
# grants mean a steadier blind-PDCCH row rate, hence a more stable slow-time grid and a less
# erratic velocity axis (vel_max was swinging 0.8-26 m/s CPI to CPI on the iperf3 runs).
for i in "${!UE_IPS[@]}"; do
  label="${UE_LABELS[$i]}"; ip="${UE_IPS[$i]}"
  [ -z "$ip" ] && continue
  echo "--- $label: generating downlink traffic to $ip for ${DURATION}s (ping + $TRAFFIC @ $IPERF_RATE) ---"
  # The SENDER always runs in the HOST netns: it only needs the existing UPF/ogstun route to the PDU
  # session subnet, independent of which netns decoded the air interface. The RECEIVER (traffic
  # terminating AT the UE) must run wherever that UE's own PDU session TUN lives -- the default
  # netns for ue1, ue_i's own netns for ue2/ue3 (see setup_ue_netns above).
  NSX=(); [ "$i" -gt 0 ] && NSX=(sudo -n ip netns exec "${UE_NETNS_BY_IDX[$i]}")
  ping -i 0.2 -s 1200 -w "$DURATION" "$ip" >"$OUT_DIR/ping_${label}.log" 2>&1 &
  if [ "$TRAFFIC" = "iperf3" ]; then
    if command -v iperf3 >/dev/null 2>&1; then
      "${NSX[@]}" iperf3 -s -B "$ip" -1 >"$OUT_DIR/iperf_server_${label}.log" 2>&1 &
      sleep 1
      iperf3 -c "$ip" -u -b "$IPERF_RATE" -t "$DURATION" >"$OUT_DIR/iperf_client_${label}.log" 2>&1 &
    else
      echo "  WARNING: iperf3 not found, DL traffic is ping-only" >&2
    fi
  else
    "${NSX[@]}" python3 "$SCRIPT_DIR/udp_dl.py" recv --bind "$ip" --port 5201 \
      --dur "$((DURATION + 5))" >"$OUT_DIR/udp_server_${label}.log" 2>&1 &
    sleep 1
    python3 "$SCRIPT_DIR/udp_dl.py" send --dst "$ip" --port 5201 --rate "$IPERF_RATE" \
      --dur "$DURATION" >"$OUT_DIR/udp_client_${label}.log" 2>&1 &
  fi
done
# Confirm traffic is ACTUALLY flowing before committing the rest of the run to it. Found the hard
# way 2026-07-29: the UDP receivers were failing to bind (EADDRNOTAVAIL, the UE address not yet on
# oaitun_ue1) and dying silently, and the gNB's own counters later showed two of three UEs had
# essentially no downlink at all (dlsch_rounds 56/59 versus 1523). Every conclusion drawn about the
# blind-PDCCH grant rate on those runs was therefore drawn on a nearly-idle cell. A 20s check costs
# nothing next to a multi-hour capture.
#
# 2026-07-30, TWO THINGS WERE WRONG WITH THAT CHECK and both are fixed below.
#
#  (a) It tested `udp_server log lines`, but udp_dl.py only prints its summary AT EXIT. That count is
#      therefore ZERO for the entire run by construction -- the check could never fire, and never
#      did. The gNB's own dlsch_rounds is the only signal here that means anything.
#  (b) It sampled ONCE, 20 s in -- during the attach burst. It cannot see a cell that goes idle
#      LATER, which is exactly what happens: on a 273 PRB run with 3 active UEs plus a 4-antenna
#      passive receiver, CPU contention starves the active UEs of uplink, the gNB logs
#      `UE <rnti> ... out-of-sync` (pucch0_DTX in the hundreds, goodput 0.00 Mbps) and STOPS
#      SCHEDULING THEM. Measured: DL died ~2-3 min into a run, after 34 MB had flowed perfectly --
#      so a 25-minute arm would be ~90 % dead cell while every startup check reported success.
#
# Replaced with a background sampler that runs for the whole capture, writes a time series, and
# warns the moment DL stalls or a UE drops out of sync. The time series is the important part: it
# makes "was the cell alive while this data was collected?" answerable AFTER the fact, which a
# one-shot check never could.
dl_total() {
  grep -oE "UE [0-9a-fx]+: dlsch_rounds [0-9]+" "$GNB_LOG" 2>/dev/null \
    | grep -oE "[0-9]+$" | paste -sd+ | bc 2>/dev/null
}

TRAFFIC_TS="$OUT_DIR/traffic_timeline.csv"
echo "wall_s,dlsch_rounds_total,delta,out_of_sync_events" > "$TRAFFIC_TS"
#
# SAMPLE PERIOD vs the gNB's STATS PERIOD: the gNB only dumps its MAC stats every ~40 s, so a 20 s
# sampler sees no new line on alternate ticks and a naive "delta == 0 means stalled" test
# false-positives on half of them (measured: 8/30 windows flagged on a cell that was demonstrably
# healthy and never went out of sync). Sample at 60 s, and require SEVERAL consecutive flat samples
# before calling it -- a real stall stays flat forever, a sampling artifact does not.
STALL_SAMPLE_S=60
STALL_CONSEC=3
(
  prev=0; t=0; stalled=0
  while [ "$t" -le "$((DURATION + STALL_SAMPLE_S))" ]; do
    sleep "$STALL_SAMPLE_S"
    t=$((t + STALL_SAMPLE_S))
    cur=$(dl_total); cur=${cur:-0}
    # `grep -c` already prints 0 when there is no match; a `|| echo 0` fallback appends a SECOND
    # line, which corrupts the CSV (it did -- stray bare "0" rows).
    oos=$(grep -c "out-of-sync" "$GNB_LOG" 2>/dev/null)
    oos=${oos:-0}
    d=$((cur - prev))
    echo "$t,$cur,$d,$oos" >> "$TRAFFIC_TS"
    if [ "$t" -ge $((STALL_SAMPLE_S * 2)) ] && [ "$d" -le 0 ]; then
      stalled=$((stalled + 1))
      if [ "$stalled" -eq "$STALL_CONSEC" ] || { [ "$stalled" -gt "$STALL_CONSEC" ] && [ $((stalled % 10)) -eq 0 ]; }; then
        echo "  *** WARNING ${t}s: DOWNLINK STALLED ($((stalled * STALL_SAMPLE_S))s flat at dlsch_rounds=$cur, out-of-sync events=$oos)."
        echo "  ***   The cell is idle -- sensing data collected from here on is not representative."
        echo "  ***   Usual cause is CPU contention starving the active UEs of uplink; reduce NUM_UE,"
        echo "  ***   RX*_NANT or IPERF_RATE. See traffic_timeline.csv."
      fi
    else
      stalled=0
    fi
    prev=$cur
  done
) &
TRAFFIC_MON_PID=$!

sleep 20
echo "--- traffic check (20s in) ---"
grep -oE "UE [0-9a-fx]+: dlsch_rounds [0-9]+" "$GNB_LOG" 2>/dev/null | tail -3 | sed 's/^/  gNB: /'
DLTOT=$(dl_total)
echo "  total dlsch_rounds so far: ${DLTOT:-0}"
[ "${DLTOT:-0}" -lt 50 ] 2>/dev/null && echo "  *** WARNING: almost no downlink -- traffic path is broken, results will be meaningless ***"
echo "  (per-20s DL timeline -> $TRAFFIC_TS; stalls are warned about as they happen)"

sleep "$DURATION"
kill "$TRAFFIC_MON_PID" 2>/dev/null

# --- 5. summary --------------------------------------------------------------------------------
echo
echo "==================== RESULTS ===================="
for i in "${!UE_ACTIVE_LOGS[@]}"; do
  label="${UE_LABELS[$i]}"; log="${UE_ACTIVE_LOGS[$i]}"
  echo "--- ACTIVE $label: attached and transmitting ---"
  grep -c "RA procedure succeeded" "$log" | sed 's/^/  RA successes:        /'
  tail -3 "$OUT_DIR/ping_${label}.log" 2>/dev/null | sed 's/^/  ping: /'
  tail -3 "$OUT_DIR/iperf_client_${label}.log" 2>/dev/null | sed 's/^/  iperf3: /'
  cat "$OUT_DIR/udp_client_${label}.log" 2>/dev/null | sed 's/^/  udp_dl: /'
  cat "$OUT_DIR/udp_server_${label}.log" 2>/dev/null | sed 's/^/  udp_dl: /'
done
grep -oP 'ulsch_rounds \K[0-9]+' "$GNB_LOG" | tail -1 | sed 's/^/  gNB-seen UL rounds (all UEs):  /'

# --- Was the cell actually ALIVE for this capture? Printed as a headline verdict, not buried in a
# file: every sensing number below is conditioned on this, and a capture whose cell died 3 minutes in
# is not a weaker result, it is a different experiment. See the sampler above for how this fails.
echo "--- DL LIVENESS (the precondition for every sensing number below) ---"
if [ -f "$TRAFFIC_TS" ]; then
  # A single flat sample is a sampling artifact (see the sampler's own comment); a STALL is
  # STALL_CONSEC consecutive flat samples, after which dlsch_rounds never moves again. Report the
  # start of the first such run, and the total DL growth, which is the honest one-number summary.
  read -r TOTAL_SAMPLES GROWTH STALL_AT OOS <<<"$(awk -F, -v need="$STALL_CONSEC" '
    NR>1 { n++; last=$2; if (first=="") first=$2; oos=$4
           if ($3<=0) { run++; if (run==need && stall=="") stall=$1-(need-1)*'"$STALL_SAMPLE_S"' }
           else run=0 }
    END { printf "%d %d %s %d", n, last-first, (stall==""?"none":stall), oos+0 }' "$TRAFFIC_TS")"
  echo "  samples: ${TOTAL_SAMPLES:-0} x ${STALL_SAMPLE_S}s   DL growth over capture: ${GROWTH:-0} dlsch_rounds"
  echo "  gNB out-of-sync events: ${OOS:-0}"
  if [ "${STALL_AT:-none}" != "none" ]; then
    echo "  *** DL STALLED from ~${STALL_AT}s -- sensing data after that point was collected on an IDLE cell ***"
    echo "  *** and is NOT comparable with data from a live one. Reduce NUM_UE / RX*_NANT / IPERF_RATE. ***"
  else
    echo "  no sustained DL stall -- cell was alive for the whole capture"
  fi
  echo "  timeline: $TRAFFIC_TS"
fi

summarize_rx() { # summarize_rx <label> <log_file> <reports_jsonl>
  local label="$1" log="$2" reports="$3"
  echo "--- PASSIVE $label: never transmitted ---"
  local ul; ul=$(grep -icE "prach|RAPROC|preamble|Msg3|PUCCH|PUSCH" "$log")
  echo "  uplink/RA log lines: $ul   (MUST be 0)"
  [ "$ul" -eq 0 ] && echo "  VERDICT: no uplink activity" || echo "  VERDICT: *** UPLINK DETECTED ***"

  echo "--- PASSIVE $label: what it captured ---"
  grep -c "SENSING" "$log" | sed 's/^/  SENSING log lines:   /'
  grep -oE "occ\[[^]]*\]" "$log" | tail -3 | sed 's/^/  CFR occupancy:       /'
  grep "blind PDCCH monitor summary" "$log" | tail -1 | sed 's/^/  blind PDCCH:         /'
  if [ -f "$reports" ]; then
    wc -l < "$reports" | sed 's/^/  DetectionReports:    /'
    python3 - "$reports" <<'PY' 2>/dev/null
import json,sys
rs=[json.loads(l) for l in open(sys.argv[1])]
if rs:
    r=rs[-1]
    print(f"  CPI duration:        {r['cpi_duration_ns']/1e9:.3f} s   "
          f"(range_res {r['range_res_m']:.2f} m, vel_max +/-{r['vel_max_mps']:.3f} m/s)")
    n=sum(1 for x in rs if any(10 <= d['bistatic_range_m']/r['range_res_m'] <= 14 for d in x['detections']))
    print(f"  CPIs with a detection in the synthetic target's range band: {n}/{len(rs)}")
    ndet=sum(len(x['detections']) for x in rs)
    naz=sum(1 for x in rs for d in x['detections'] if 'azimuth_deg' in d)
    if ndet>0:
        print(f"  detections carrying azimuth (AoA):    {naz}/{ndet}")
PY
  fi
  grep "SENSING_CHANNEL gt:" "$log" | tail -1 | sed 's/^/  ground truth: /'
}
for i in "${!RX_LABELS[@]}"; do
  summarize_rx "${RX_LABELS[$i]}" "${RX_LOGS[$i]}" "${RX_REPORTS[$i]}"
done

# --- 6. fusion ------------------------------------------------------------------------------------
# Real-wall-clock pairing + isac-track replay. NUM_RX==2 uses tests/sensing_sim's
# merge_receivers_walltime.py directly (exact pairwise pairing, its original design). NUM_RX>2 uses
# this dir's own merge_receivers_walltime_n.py instead of chaining merge_receivers_walltime.py
# pairwise -- chaining was tried first and found live to be lossy (each chain step only keeps
# records that ALREADY paired in the previous step, so it compounds losses instead of independently
# checking each receiver against the anchor). MIN_RX=2 (of NUM_RX) matches CLAUDE.md's "isac-track
# needs >=2 pairs" rule -- a group is emitted once at least 2 receivers have a nearby CPI, not only
# when ALL of them do.
echo "--- FUSION ($NUM_RX receivers) ---"
MISSING=0
for r in "${RX_REPORTS[@]}"; do [ -f "$r" ] || MISSING=1; done
if [ "$MISSING" -eq 1 ]; then
  echo "  SKIPPED: one or more report files missing (${RX_REPORTS[*]})"
elif [ ! -x "$ISAC_TRACK" ]; then
  echo "  SKIPPED: isac-track not found/executable at $ISAC_TRACK"
  echo "  build it with: cd /home/sens/NICOLA/repos/isac && cargo build --release -p isac-track"
else
  # Cross-receiver RNTI consistency gate (see rnti_gate.py's header): drop blind-PDCCH-sourced CPIs
  # whose RNTI wasn't ALSO seen by a different receiver at the same wall-clock time -- a much
  # stronger noise-floor test than any single receiver's own persistence gate, since it needs no
  # tuned threshold. csi_rs-only CPIs pass through untouched. Feed the GATED reports into the merge
  # below instead of the raw ones.
  MERGE_SPECS=()
  if [ "$NUM_RX" -eq 1 ]; then
    # The RNTI gate is a CROSS-RECEIVER consistency test: it keeps a blind-PDCCH CPI only if some
    # OTHER receiver saw the same RNTI at the same wall-clock time. With one receiver that question
    # has no answer and the gate rejects every blind-sourced CPI BY CONSTRUCTION, not on evidence --
    # measured dropping 39 of 67 CPIs on a single-receiver run, leaving only the csi_rs-only ones.
    # So it is skipped entirely here; there is nothing for it to cross-check against.
    GATED_REPORTS=("${RX_REPORTS[@]}")
    echo "  rnti_gate: SKIPPED (single receiver -- a cross-receiver test has no second opinion)"
  else
    RNTI_GATE_SPECS=()
    for i in "${!RX_REPORTS[@]}"; do RNTI_GATE_SPECS+=("${RX_REPORTS[$i]}:${RX_LOGS[$i]}"); done
    GATED_REPORTS_STR=$(python3 "$SCRIPT_DIR/rnti_gate.py" "$OUT_DIR" "${RNTI_GATE_SPECS[@]}" 2> >(sed 's/^/  rnti_gate: /' >&2))
    read -r -a GATED_REPORTS <<< "$GATED_REPORTS_STR"
  fi
  for i in "${!GATED_REPORTS[@]}"; do MERGE_SPECS+=("${GATED_REPORTS[$i]}@${RX_LOGS[$i]}"); done

  MERGED="$OUT_DIR/fused_reports.jsonl"
  if [ "$NUM_RX" -eq 1 ]; then
    # Single receiver: there is nothing to merge, so the (gated) reports ARE the fusion input.
    cp -f "${GATED_REPORTS[0]}" "$MERGED"
    echo "  merge: single receiver, $(wc -l < "$MERGED") CPIs passed through unmerged"
  elif [ "$NUM_RX" -eq 2 ]; then
    python3 "$SCRIPT_DIR/../sensing_sim/merge_receivers_walltime.py" \
      "${GATED_REPORTS[0]}" "${GATED_REPORTS[1]}" "$MERGED" 2>&1 | sed 's/^/  merge: /'
  else
    python3 "$SCRIPT_DIR/merge_receivers_walltime_n.py" \
      "$MERGED" 2 auto "${MERGE_SPECS[@]}" 2>&1 | sed 's/^/  merge: /'
  fi
  TRACKS="$OUT_DIR/fused_tracks.jsonl"
  if [ -s "$MERGED" ]; then
    # AoA-gated fusion (PHASE3_AOA_MULTISTATIC_HANDOVER.md's redundancy gate, previously only wired
    # into tests/sensing_sim's attached-UE arms, live-verified there taking 2rx precision 16%->100%
    # on the SAME class of data). --birth-min-pos-redundancy 1 requires a fix be checkable (>=2
    # pairs, or 1 pair + bearing when a receiver has an array); --birth-max-pos-chi2 9 (~3 sigma)
    # then rejects inconsistent ones. Both are dimensionless and AUTOMATICALLY INERT when a fix has
    # no redundancy (isac-track's own guarantee), so this is safe to pass unconditionally even for
    # receivers running single-antenna/no-AoA (RX*_NANT=1) -- it just does nothing for them.
    # Birth flags depend on how many receivers there are, and getting this wrong is silent:
    # redundancy is (pairs + bearings - 2), so ONE pair plus its bearing scores exactly 0 and
    # --birth-min-pos-redundancy 1 would refuse EVERY birth -- turning a single-receiver arm into
    # "no tracks" rather than into a result. (Measured on the attached-UE harness: the 1-pair arm
    # emits 135 track updates without that flag and 0 with it.) A single receiver instead needs
    # --birth-single-pair-bearing, which localises by ray-ellipse intersection; it buys COVERAGE,
    # not ghost rejection, because a harmonic ghost shares its parent's bearing.
    if [ "$NUM_RX" -eq 1 ]; then
      BIRTH_FLAGS=(--birth-max-pos-chi2 9 --birth-single-pair-bearing)
    else
      BIRTH_FLAGS=(--birth-max-pos-chi2 9 --birth-min-pos-redundancy 1)
    fi
    "$ISAC_TRACK" --out "$TRACKS" "${BIRTH_FLAGS[@]}" ${ISAC_EXTRA_ARGS:-} \
      replay "$MERGED" >"$OUT_DIR/isac_track.log" 2>&1
    NTRACKS=$(wc -l < "$TRACKS" 2>/dev/null || echo 0)
    echo "  fused tracks: $NTRACKS   (see $TRACKS, $OUT_DIR/isac_track.log)"
    if [ "$NTRACKS" -gt 0 ]; then
      NCONF=$(python3 -c "
import json
n=sum(1 for l in open('$TRACKS') if json.loads(l).get('status')=='confirmed')
print(n)
" 2>/dev/null)
      echo "  confirmed-status track updates: ${NCONF:-?}"
      # Ground-truth precision, mirroring tests/sensing_sim's score_world_tracks.py TOL=15m metric.
      python3 "$SCRIPT_DIR/score_passive_tracks.py" "$TRACKS" "${RX_LOGS[0]}" 2>&1 | sed 's/^/  /'
      # STANDING REQUIREMENT: every capture gets plotted against ground truth -- fused tracks and
      # speed, plus per-receiver detections. Numbers alone hid a 33x time-base error and a whole
      # class of bearing outliers that were obvious the moment they were drawn.
      python3 "$SCRIPT_DIR/plot_fused_tracks.py" "$TRACKS" "${RX_LOGS[0]}" \
        "$OUT_DIR/fused_tracks_vs_gt.png" "$(basename "$OUT_DIR")" 2>&1 | sed 's/^/  plot: /'
      for i in "${!RX_LABELS[@]}"; do
        python3 "$SCRIPT_DIR/plot_est_vs_gt.py" "${RX_REPORTS[$i]}" "${RX_LOGS[$i]}" \
          "$OUT_DIR/est_vs_gt_${RX_LABELS[$i]}.png" "${RX_LABELS[$i]}" 2>&1 | sed 's/^/  plot: /'
      done
    fi
  else
    echo "  SKIPPED isac-track: no paired CPIs (receiver CPI timestamps never landed within tolerance)"
  fi
fi
echo "================================================"
echo "logs: $GNB_LOG, ${UE_ACTIVE_LOGS[*]}, ${RX_LOGS[*]}"
