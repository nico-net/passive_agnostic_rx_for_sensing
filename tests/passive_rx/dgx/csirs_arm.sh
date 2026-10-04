#!/bin/bash
# CSI-RS rfsim arm. Run only in an orchestrated quiet DGX slot.
# Usage: MODE=off|blind|idsweep [RXANT=1|4] [GNBCONF=...] csirs_arm.sh OUTDIR SECONDS
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
MODE=${MODE:-blind}
case "$MODE" in
  off) export ISAC_CSIRS_BLIND=0 ISAC_CSIRS_BLIND_IDSWEEP=0 ;;
  blind) export ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=0 ;;
  idsweep) export ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1 ;;
  *) echo 'MODE must be off, blind, or idsweep' >&2; exit 2 ;;
esac
RXANT=${RXANT:-1}
case "$RXANT" in 1|4) ;; *) echo 'RXANT must be 1 or 4' >&2; exit 2 ;; esac
export ISAC_PDCCH_TIMING=1
export RXEXTRA="${RXEXTRA:-} --ue-nb-ant-rx $RXANT"
exec "$DIR/rfsim_arm.sh" "$@"
