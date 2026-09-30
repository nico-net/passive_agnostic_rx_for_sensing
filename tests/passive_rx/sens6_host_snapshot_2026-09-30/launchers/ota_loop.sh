#!/bin/bash
# OTA capture: 60 s settle, no MPM restart. Only when an attempt dies at stream start (RFSTALL at
# RX_START / device load failure) is MPM restarted, and only then is the settle 180 s. Max 3 attempts.
cd /home/sens/NICOLA/multirx-clean-adaptive
DUR=${DUR:-600}; RXG=${RXG:-50}; EXTRA=${EXTRA:-}; MPM=${MPM:-0}
# Wide 256QAM decode (MCS 25): chest time interpolation + SFO phase-ramp correction (DL CRC 50 -> 83 %, wide MCS-25 0 -> 80 %, 2026-09-24)
export EXTRA_XENV="ISAC_CHEST_TINTERP=1 ISAC_SFO_CORRECT=1 ${EXTRA_XENV:-}"
for attempt in 1 2 3; do
  if [ "$MPM" = 1 ]; then ssh root@${MGMTA:-192.168.1.140} systemctl restart usrp-hwd; echo "attempt $attempt: MPM restarted $(date +%T), settling 180 s"; sleep 180
  else echo "attempt $attempt: settling 60 s $(date +%T)"; sleep 60; fi
  ping -c2 -W2 ${MGMTA:-192.168.1.140} >/dev/null && echo "X410 ping ok" || echo "X410 PING FAILED"
  TEMPLATE=${TEMPLATE:-$PWD/tests/passive_rx/ota/sensing_ota_manual.conf.template} SCAN=0 SSB=150 INITIALFO=-15000 NANT=4 \
    bash tests/passive_rx/run_sensing.sh --dur "$DUR" --rxg "$RXG" --mgmt ${MGMTA:-192.168.1.140} $EXTRA --survey $PWD/tests/passive_rx/ota/survey.json
  L=$(ls -dt /home/sens/NICOLA/captures/sense_* | head -1)/run.log
  if grep -q "RFSTALL USRP_RX_START\|openair0_device_load" "$L" && ! grep -q RFCENSUS "$L"; then
    echo "attempt $attempt: start-up stall, next attempt restarts MPM $(date +%T)"; MPM=1; continue
  fi
  break
done
echo "RUN_SENSING EXITED $(date +%T)"
