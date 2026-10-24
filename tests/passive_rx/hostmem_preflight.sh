#!/bin/bash
# Host-RAM preflight for a receiver launch (replaces fixed free-memory guards such as the DGX ota.sh "MemAvailable >= 48 GB",
# which is meaningless on a 14.8 GB host like sens6).
#
# Runs the receiver's OWN memory plan: nr-uesoftmodem with ISAC_HOST_MEM_PLAN_ONLY=1 sizes every big host allocation from the
# host-RAM budget exactly as a real start would (USRP ring reservation, PDSCH queue, recovery holders, CSI-RS rings, time-domain
# pool, pinned mirrors, GrantWork pool), prints "SENSING: HOST_MEM PLAN verdict=OK|REFUSE ..." and exits before the radio is
# opened (no X410 claim, no stream). Exit 0 = the plan fits, 7 = refused (same code as the DGX guard), 3 = no verdict.
#
# usage: hostmem_preflight.sh <path/to/nr-uesoftmodem> <the receiver arguments, exactly as for the real run>
#   Pass the same environment as the real run (ISAC_USRP_RING_MS etc.: export it before calling, or prefix `env A=B`).
#   On a shared host run it under the same memory cap as the receiver, e.g.
#     systemd-run --user --scope -q -p MemoryMax=7G -p MemorySwapMax=1G choom -n 1000 -- hostmem_preflight.sh ...
#   (the budget reads the cgroup v2 memory.max, so the verdict is the one for that cap).
# Environment: PREFLIGHT_TIMEOUT (s, default 120). Stops the receiver with SIGINT only.
set -u
[ $# -ge 1 ] || { sed -n 2,16p "$0"; exit 2; }
BIN=$1; shift
[ -x "$BIN" ] || { echo "hostmem_preflight: $BIN is not executable" >&2; exit 2; }
OUT=$(mktemp -d /tmp/hostmem_preflight.XXXXXX)
ma=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
( cd "$OUT" && ISAC_HOST_MEM_PLAN_ONLY=1 timeout -s INT "${PREFLIGHT_TIMEOUT:-120}" "$BIN" "$@" > "$OUT/plan.log" 2>&1 )
rc=$?
line=$(grep -a -m1 "SENSING: HOST_MEM PLAN" "$OUT/plan.log")
refuse=$(grep -a -m3 "HOST_MEM REFUSE\|HOST_MEM .* -> \|below the" "$OUT/plan.log")
echo "hostmem_preflight: MemAvailable at start ${ma} MB, receiver rc=$rc, log $OUT/plan.log"
[ -n "$refuse" ] && echo "$refuse"
if [ -z "$line" ]; then echo "hostmem_preflight: no verdict (see the log)"; exit 3; fi
echo "$line"
case "$line" in *verdict=OK*) exit 0 ;; *) exit 7 ;; esac
