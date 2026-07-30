#!/usr/bin/env bash
# Loops an SC16Q11 waveform (convert_iq.py to-sc16q11 output) out of a bladeRF continuously.
# Verify the exact `set frequency`/`set samplerate`/`set gain` argument order against your
# installed bladeRF-cli first (`bladeRF-cli --help-interactive`) -- this uses the documented
# `set <param> <rx|tx> <value>` form as of libbladeRF 1.10.
set -euo pipefail

usage() {
  echo "usage: $0 <waveform.sc16q11.bin> <freq_hz> <rate_hz> <gain_db> [device_args] [channel]"
  echo "  e.g.: $0 tx_waveform.bin 3400000000 30720000 40 '' 1"
  exit 1
}
[ $# -ge 4 ] || usage

FILE=$1
FREQ=$2
RATE=$3
GAIN=$4
DEV=${5:-}
CHAN=${6:-1}

DEV_OPT=()
[ -n "$DEV" ] && DEV_OPT=(-d "$DEV")

echo "TX (bladeRF): file=$FILE freq=$FREQ rate=$RATE gain=$GAIN channel=$CHAN (repeat=0, i.e. loop until Ctrl-C)"
bladeRF-cli "${DEV_OPT[@]}" \
  -e "set frequency tx ${FREQ}" \
  -e "set samplerate tx ${RATE}" \
  -e "set bandwidth tx ${RATE}" \
  -e "set gain tx ${GAIN}" \
  -e "tx config file=${FILE} format=bin repeat=0 channel=${CHAN}" \
  -e "tx start" \
  -e "tx wait"
