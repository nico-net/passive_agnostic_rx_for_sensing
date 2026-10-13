#!/bin/bash
# si_ab.sh TAG PCI CAPNAME CAPC RXC SSB FO [ENV=...]... : one replay of newcells_tiled/CAPNAME with the t26b_pci4-style env + ISAC_TARGET_PCI=PCI (PCIS2 2026-10-13).
# Prints the SI-RNTI CRC census of the run. Takes heavy.lock (exclusive) through timacq_nant.sh. Replay only, no radio.
set -u
D=$(cd "$(dirname "$0")/../../.." && pwd); TAG=$1; PCI=$2; CN=$3; CAPC=$4; RXC=$5; SSB=$6; FO=$7; shift 7
case $CN in /*) CAP=$CN;; *) CAP=/home/nicola/NICOLA/ota_runs/newcells_tiled/$CN;; esac; J=$CAP/capture_info.json
export CAP CAPC RXC SSB FO SPEED=${SPEED:-0.25} B=${B:-$D/cmake_targets/ran_build/build_t26} ENVFILE=${ENVFILE:-/home/nicola/NICOLA/wt/rr-orchestration/ota/t26b_pci4.env}
export CAP_SAMPLES=$(python3 -c "import json;print(json.load(open('$J'))['samples_per_channel'])") CAP_TICK=$(python3 -c "import json;print(json.load(open('$J'))['first_sample_tick'])")
"$D/tests/passive_rx/survey/timacq_nant.sh" "$TAG" ISAC_TARGET_PCI=$PCI "$@" >/dev/null 2>&1; RC=$?
R=$(cat "$D/replays/last"); echo "== $TAG rc=$RC $R"
sed 's/\x1b\[[0-9;]*m//g' "$R/run.log" > "$R/clean.log"
python3 - "$R/clean.log" <<'PY'
import re,sys,collections
t=open(sys.argv[1]).read().splitlines(); ev=[l for l in t if 'EVMGRANT rnti=0xffff' in l]
ok=sum(' crc=1 ' in l for l in ev); sn=[float(re.search(r' snr=(-?[\d.]+)',l).group(1)) for l in ev if re.search(r' snr=(-?[\d.]+)',l)]
print(f'SI-RNTI EVMGRANT: {len(ev)} crc_ok {ok} ({100*ok/max(len(ev),1):.1f} %) mean snr {sum(sn)/max(len(sn),1):.2f} dB')
print('SI_SOFTCOMBINE lines:',sum('SI_SOFTCOMBINE' in l for l in t),' SIB1 decoded lines:',sum(('SIB1' in l and ('decoded' in l or 'DERIV' in l)) for l in t))
rv=collections.Counter(re.search(r' rv=(\d)',l).group(1) for l in ev if re.search(r' rv=(\d)',l)); print('rv census',dict(rv))
print('nID GPU passes:',sum('PDCCH_NID_GPU pass' in l and 'stage1' in l for l in t),' CELL_STATE:',[l[l.index('CELL_STATE'):][:200] for l in t if 'CELL_STATE' in l][:3],' SI stats:',[l[l.index('SI_SOFTCOMBINE'):] for l in t if 'SI_SOFTCOMBINE stats' in l][-1:],' carrier:',sorted(set(re.findall(r'carrier=(\w+)',' '.join(t))))[:3])
last=[l for l in t if 'PDSCHQ queued' in l][-1:] ; print(last[0][-220:] if last else '')
PY
