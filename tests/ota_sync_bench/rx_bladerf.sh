#!/usr/bin/env bash
# Captures <duration> seconds of IQ from a bladeRF to an SC16Q11 file (convert with
# convert_iq.py from-sc16q11 before feeding process_capture.py). Same syntax caveat as
# tx_bladerf.sh -- verify against your installed bladeRF-cli.
set -euo pipefail

usage() {
  echo "usage: $0 <out.sc16q11.bin> <freq_hz> <rate_hz> <gain_db> <duration_s> [device_args] [channel]"
  echo "  e.g.: $0 rx_capture.bin 3400000000 30720000 30 10 '' 1"
  exit 1
}
[ $# -ge 5 ] || usage

OUT=$1
FREQ=$2
RATE=$3
GAIN=$4
DUR=$5
DEV=${6:-}
CHAN=${7:-1}

N=$(awk -v r="$RATE" -v d="$DUR" 'BEGIN{printf "%d", r*d}')

DEV_OPT=()
[ -n "$DEV" ] && DEV_OPT=(-d "$DEV")

echo "RX (bladeRF): out=$OUT freq=$FREQ rate=$RATE gain=$GAIN channel=$CHAN n=$N samples (~${DUR}s)"
bladeRF-cli "${DEV_OPT[@]}" \
  -e "set frequency rx ${FREQ}" \
  -e "set samplerate rx ${RATE}" \
  -e "set bandwidth rx ${RATE}" \
  -e "set gain rx ${GAIN}" \
  -e "rx config file=${OUT} format=bin n=${N} channel=${CHAN}" \
  -e "rx start" \
  -e "rx wait"
