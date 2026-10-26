#!/bin/bash
cd /home/sens/NICOLA/multirx-clean-adaptive
if [ "${MPM:-0}" = 1 ]; then ssh root@192.168.1.140 systemctl restart usrp-hwd; echo "MPM restarted $(date +%T), settling 180 s"; sleep 180
else echo "settling 60 s $(date +%T)"; sleep 60; fi
ping -c2 -W2 192.168.1.140 >/dev/null && echo "X410 ping ok" || echo "X410 PING FAILED"
TEMPLATE=$PWD/tests/passive_rx/ota/sensing_ota_manual.conf.template SCAN=0 SSB=150 INITIALFO=-15000 NANT=4 \
  bash tests/passive_rx/run_sensing.sh --dur 150 --debug --rxg ${RXG:-50} --survey $PWD/tests/passive_rx/ota/survey.json
echo "RUN_SENSING EXITED rc=$? $(date +%T)"
