# Runbook: esperimenti di laboratorio del ricevitore passivo (sens6 + X410, cella sens4)

Stato: 2026-10-26. Sorgenti: `tests/passive_rx/ota/lab_sens4_run.{sh,cfg}`, `tests/passive_rx/sens6_ota/run_ota_s6.sh`,
`ota_lab273.s6.env`, `ue.passive.ota.lab273.s6.cfg`, `tools/mobisys/*`, report `docs/superpowers/reports/csd/OTA-*.md`,
`WIDE-RANK4-20261025.md`, `PROJECT_MEMORY.md` §15.00 e §26.
Etichette: `[OTA MEASURED sens6, cella lab]`, `[MEASURED, sens6 rfsim]`, `[CODE-READ]`, `[HYPOTHESIS]`. Mai sommare host, celle o bande diverse.
`da verificare` = non presente nelle fonti.

Variabili usate sotto (build integrata del 2026-10-26: ramo `sens6/integ-1026-fix`, commit `c0736a228e`):

```
WT=/home/sens/NICOLA/agn-wt/ul-fit                 # worktree (editing e build)
B=$WT/build_ulfit                                  # build dir (per build e check)
FZ=/home/sens/NICOLA/agn-wt/integ-frozen           # copia congelata per le run on-air (sezione 4.0)
```

---

## 1. Architettura e host

| host | ruolo | dettagli |
|---|---|---|
| sens6 | ricevitore passivo | x86, Core Ultra 5 235, 14 CPU, isolcpus 1-2, 14.8 GB RAM, RTX 4060 Ti 8 GB (CUDA 12.4), kernel 7.0.0-34-realtime |
| X410 | radio (su sens6) | seriale 327B872, board 327488F, immagine UC_200, MPM 6.1, FPGA 11.0, UHD 4.10.0; data/mgmt `192.168.20.2` |
| NIC | link X410 | `enp2s0f1np1`, 100 Gb/s, MTU 9000, anelli 8192; IPv4 `192.168.20.65/24` (+ `192.168.20.1/24` nel report, non persistente) |
| sens4 | gNB OCUDU + O-RU LITEON | n78, 3450 MHz (dl_arfcn 630000), 273 PRB, SCS 30, PCI 2, TDD 7D/2U, 4T4R, PLMN 00106 |
| OPPO | telefono 1 | `ff1ecb98`, adb su sens4, iperf3 `iperf3` (in PATH), porta 5204 |
| Samsung | telefono 2 | `R5CX11CBYJP`, adb `/usr/bin/adb` su sensnuc3, iperf3 `/data/local/tmp/iperf3`, porta 5205 |
| iperf3 server | sens4 | `10.42.0.1:5204` e `10.42.0.1:5205`. Mai la porta 5201 (di un altro server) |

- Orologio sens4 circa 1 min indietro rispetto a sens6: il ground truth si allinea per SFN, non per ora di parete (offset SFN, es. `-122880` slot in una run) `[OTA MEASURED sens6]`.
- Configurazione gNB base (sola lettura, mai modificare): `/home/sens/ABS/AgenticBS/logs/configs/gnb_ofh_ota_tdd30_100mhz_liteon.1e3123cc87ac.yml` (su sens4).
- Il gNB e i suoi log validano soltanto: non sono mai input del ricevitore.

---

## 2. Preparazione host (dopo ogni reboot, non persistente)

```
sudo sysctl -w net.core.netdev_max_backlog=250000 net.core.rmem_default=62500000 net.core.rmem_max=62500000
for i in $(ls /sys/class/net/enp2s0f1np1/device/msi_irqs); do echo 0 | sudo tee /proc/irq/$i/smp_affinity_list; done
echo 0 | sudo tee /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages     # libera gli hugepage 1G (DPDK non usato)
sudo ip addr add 192.168.20.65/24 dev enp2s0f1np1                             # solo se l'indirizzo manca
cd $WT && ENVFILE=tests/passive_rx/sens6_ota/ota_lab273.s6.env tests/passive_rx/sens6_ota/run_ota_s6.sh check
```

- Il `check` deve mostrare `== preflight: 0 fatal, 0 warnings`. Il `check` non cambia l'host e non reclama l'X410 (usa solo ping e `uhd_find_devices`).
- Il `check` esce con 0 se non ci sono FATAL; i WARN vanno comunque azzerati prima di una run. Il launcher in `run` rifiuta di partire con warning se non c'è `FORCE=1`.
- Il `check` stampa i comandi `sudo` che servono ancora: lanciarli a mano, lo script non li esegue.

---

## 3. Build

Lock e cgroup:

| cosa | come | fonte |
|---|---|---|
| build | `systemd-run --user --scope -p MemoryMax=...` (ninja `-j3` in scope da 5G; ctest in scope da 4G) | OTA-READY §2 |
| lock build | `flock -s /tmp/td_measure.lock` (condiviso) | PROJECT_MEMORY §26 |
| lock build (sens6) | `flock -x /tmp/sens6_build.lock` (esclusivo, una build alla volta) | in uso da tutti gli agenti il 2026-10-25 |
| mai | build con `nr-uesoftmodem` attivo | CLAUDE.md |

Configurazione (una volta, il build dir è `$WT/build_ota`, default di `run_ota_s6.sh`):

```
pgrep -x nr-uesoftmodem && echo "STOP: ricevitore attivo" || echo "ok: nessun ricevitore"
cmake -S $WT -B $WT/build_ota -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DOAI_USRP=ON \
      -DENABLE_ISAC_SENSING=ON -DENABLE_LDPC_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=89
```

Build (target base dal report + tutti i plugin GPU dell'elenco):

```
flock -s /tmp/td_measure.lock flock -x /tmp/sens6_build.lock \
 systemd-run --user --scope --quiet -p MemoryMax=5G -p MemorySwapMax=1G choom -n 1000 -- ninja -C $B -j3 \
  nr-uesoftmodem oai_usrpdevif params_libconfig \
  pdcch_nid_gpu ldpc_cuda csirs_disc_gpu nr_csirs_disc_gpu_sweep pdcch_gpu pdsch_gpu polar_gpu td_cb0_gpu
```

- Tutti i plugin GPU sono target `MODULE` in `openair1/PHY/CODING/CMakeLists.txt` `[CODE-READ]`.
- Un binario senza plugin GPU disattiva silenziosamente la discovery: run `dd1_lab_20261025_133712` → `PDCCH_NID_GPU init failed`, nessun CORESET, recall DCI ~15 % `[OTA MEASURED sens6]`. Controllare `ls $B/*.so | wc -l` (15 senza rfsimulator).
- Dopo la build: `ls -l $WT/build_ota/nr-uesoftmodem $WT/build_ota/liboai_device.so` (il `check` verifica che nessun sorgente sia più nuovo del binario).
- Build 2026-10-26: usare `$WT/build_ulfit` (worktree `ul-fit`, commit `c0736a228e`) al posto di `build_ota` in questa sezione e nei comandi sotto.

ctest (sens6, da report OTA-READY §2):

```
cd $WT/build_ota
systemd-run --user --scope -p MemoryMax=4G -- ctest -j2 -E "gpu|cuda"     # CPU
systemd-run --user --scope -p MemoryMax=4G -- ctest -j1 -R "gpu|cuda"     # GPU, GPU libera
```

Test noti che falliscono per contesa (non reali): `nr_td_sim_test`, `test_nr_ssb_rate_match_prod`, `test_vrtsim_cirdb`, `test_nr_td_cb0_batch(_discrete)`, `nr_cuup_functional_test`, `test_ldpc_cb0_cuda(_discrete)`, `test_nr_gpu_pass` (serial o GPU libera). `test_nr_ldpc_cb0_cuda(_discrete)` fallisce su sens6 `BatchSizesAreInvariant`, non ancora bisecato (WIDE-RANK4 §Also found).

---

## 4. Avvio di un esperimento

### 4.0 Copia congelata per le run on-air (obbligatoria)

Il worktree di lavoro cambia mentre gli agenti editano i sorgenti: il preflight rifiuta il binario con "source newer than the binary". Per le run usare una copia congelata:

```
git worktree add --detach /home/sens/NICOLA/agn-wt/integ-frozen c0736a228e
mkdir -p /home/sens/NICOLA/agn-wt/integ-frozen/build_frozen
cp --remove-destination $WT/build_ulfit/nr-uesoftmodem $WT/build_ulfit/*.so /home/sens/NICOLA/agn-wt/integ-frozen/build_frozen/
cd /home/sens/NICOLA/agn-wt/integ-frozen
B=/home/sens/NICOLA/agn-wt/integ-frozen/build_frozen tests/passive_rx/ota/lab_sens4_run.sh <ARM>
```

- Le copie vanno fatte DOPO il checkout. `cp --remove-destination` crea file reali, non link simbolici.
- Lanciare sempre `lab_sens4_run.sh` da `integ-frozen` con `B=.../build_frozen`.

### 4.1 Comando unico (consigliato)

Sempre prima il dry-run:

```
cd $WT
tests/passive_rx/ota/lab_sens4_run.sh --dry-run <ARM>      # solo check di sola lettura, nessuna modifica
tests/passive_rx/ota/lab_sens4_run.sh <ARM>                # run reale: DUR=300 s di default
```

Varianti (ogni chiave di `lab_sens4_run.cfg` si può sovrascrivere dall'ambiente):

```
RXG=36 tests/passive_rx/ota/lab_sens4_run.sh <ARM>                        # guadagno RX (default cfg via env file: 49)
DUR=600 tests/passive_rx/ota/lab_sens4_run.sh <ARM>                       # durata ricevitore in secondi
B=$WT/build_ota GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c /home/sens/NICOLA/gnbLogs/armcfg/gnb_L1.yml" \
  RXG=36 tests/passive_rx/ota/lab_sens4_run.sh <ARM>                      # gNB per-run (vedi sezione 5)
PHONES="oppo:sens4:adb:ff1ecb98:iperf3:5204" tests/passive_rx/ota/lab_sens4_run.sh <ARM>   # un solo telefono
PHASE_BW="-b 100M" tests/passive_rx/ota/lab_sens4_run.sh <ARM>            # limite di banda iperf3 (carico)
```

- `RXG` e `B` arrivano al launcher per ereditarietà dell'ambiente; `RXG` vince sull'env file.
- Il gNB base del cfg è `sudo taskset -c 2-11 .../gnb -c <yml base>`; `GNB_OVERRIDES` aggiunge sempre log e pcap per-run (`@LOG@`, `@PCAP@`).
- `RXG=36` è il migliore su questa cella: sweep 2026-10-25 (8 run `g{49,42,36,30}_lab_20261025_1*`) `[OTA MEASURED sens6]`: 49 clippa (DL 0.08-0.13 % a fondo scala); 42→36 segnale e rumore scendono entrambi di 6 dB (SNR DM-RS 30 dB costante); 30 perde ~5 dB di SNR UL (rumore termico).
- Il guadagno 49 fa clippare i slot DL (ADCCLASS, clip 0.1-0.4 %) `[OTA MEASURED sens6]`. È configurazione, non codice.

Sequenza che lo script esegue (per capire dove si ferma):

1. Check di sola lettura: lock testbed idle (nota `stop_all...`), nessun gNB, telefoni su adb, preflight ricevitore.
2. Lock `/tmp/td_measure.lock` (esclusivo) poi lock testbed su sens4 (tmux `s6lab_lock`).
3. gNB in tmux `s6lab_gnb`, attesa `N2: Connection to AMF.*completed` (max 180 s).
4. Telefoni: PLMN 00106 e indirizzo 10.45.x, altrimenti un toggle modo aereo; server iperf3 looped (`s6lab_iperf`).
5. Loop di traffico DL (`-R`) / UL alternati ogni 30 s; poi il ricevitore (10 s dopo il traffico).
6. Teardown: SIGINT al ricevitore, poi iperf3 dei telefoni, server, gNB (`C-c`), modo aereo ripristinato, lock rilasciato, log raccolti, ground truth.

### 4.2 Alternative manuali per componente

**gNB (sens4)**, equivalente al comando dello script:

```
ssh sens4 "mkdir -p /home/sens/NICOLA/gnbLogs && tmux new-session -d -s s6lab_gnb \
  \"sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c <YML> \
  log --filename=<LOG> --mac_level=info pcap --mac_enable=true --mac_type=dlt --mac_filename=<PCAP> 2>&1 | tee -i <STDOUT>\""
ssh sens4 "grep -E 'N2: Connection to AMF.*completed' <STDOUT>"        # pronto
ssh sens4 "tmux send-keys -t s6lab_gnb C-c"                             # stop (mai kill -9)
```

**Telefoni**:

```
# OPPO (da sens4)
adb -s ff1ecb98 shell getprop gsm.operator.numeric                    # atteso 00106
adb -s ff1ecb98 shell ip -4 addr                                       # atteso 10.45.x
adb -s ff1ecb98 shell cmd connectivity airplane-mode enable; sleep 3; adb -s ff1ecb98 shell cmd connectivity airplane-mode disable
adb -s ff1ecb98 shell iperf3 -c 10.42.0.1 -p 5204 -t 30 -i 5 -R        # DL; senza -R = UL
# Samsung (da sensnuc3)
/usr/bin/adb -s R5CX11CBYJP shell /data/local/tmp/iperf3 -c 10.42.0.1 -p 5205 -t 30 -i 5 -R
```

**Server iperf3 (sens4)**:

```
tmux new-session -d -s s6lab_iperf "while :; do iperf3 -s -1 -B 10.42.0.1 -p 5204; done"
tmux new-window  -t s6lab_iperf "while :; do iperf3 -s -1 -B 10.42.0.1 -p 5205; done"
```

**Ricevitore X410 (sens6)**, solo launcher:

```
cd $WT
ENVFILE=tests/passive_rx/sens6_ota/ota_lab273.s6.env tests/passive_rx/sens6_ota/run_ota_s6.sh dry-run <ARM>   # stampa il comando, non parte
ENVFILE=tests/passive_rx/sens6_ota/ota_lab273.s6.env tests/passive_rx/sens6_ota/run_ota_s6.sh run <ARM>       # preflight + 1 run di DUR s
```

- Il `run` prende da solo `/tmp/td_measure.lock` (esclusivo) se `TD_LOCK_HELD` non è 1. Il ricevitore gira in `systemd-run --user --scope -p MemoryMax=min(7G, MemAvailable-0.5G) ... choom -n 1000 -- sudo -n env ... timeout -s INT $DUR`.
- Non parte sotto 7 GB di `MemAvailable`. Output in `$OUTBASE/<ARM>_<ts>` (default `/home/sens/NICOLA/captures`).
- Stop: solo SIGINT. Se il ricevitore non risponde: `sudo -n kill -INT <pid>` (pid di `pgrep -x nr-uesoftmodem`). Mai SIGKILL, mai `pkill -f` (PROJECT_MEMORY §26).
- Watchdog del launcher: stallo log 60 s -> SIGINT; ancora vivo 30 s dopo il SIGINT di DUR -> secondo SIGINT. Eventi in `watchdog.txt`.

---

## 5. Configurazioni gNB per gli esperimenti (arm rank / MCS)

Le copie per-run stanno su **sens4** (non su sens6): `/home/sens/NICOLA/gnbLogs/armcfg/` (sens4). Ogni copia cambia solo `pdsch.max_rank`, `pusch.max_rank` e `max_ue_mcs`; la yml base resta intatta (sha256 `1e3123cc…`).

| arm | file (su sens4) | DL rank | MCS | esito in fonte |
|---|---|---|---|---|
| B0 | yml base (nessuna copia) | 4 | max 25 (qam256) | `ri=4`; PDSCH CRC 0.05 / 0.06 (phone A/B) |
| L1 | `armcfg/gnb_L1.yml` | 1 | 25 | `ri=1`; CRC 0.71 / 0.84 |
| L1M | `armcfg/gnb_L1M.yml` | 1 | max 16 | `ri=1`, MCS 16; CRC 0.52 / 0.84 |
| L2 | `armcfg/gnb_L2.yml` | 2 | 25 | layout non risolto mai (0 TB) |
| D1 / D1M / D2 / D2M | `armcfg/gnb_D1.yml` … `gnb_D2M.yml` (creati 2026-10-25; solo DL, UL invariato) | 1 / 1 / 2 / 2 | 27 / 16 / 27 / 16 | in corso (serie `integ.sh`) |

Uso: `GNB_CMD="sudo taskset -c 2-11 /home/sens/ABS/ocudu/build/apps/gnb/gnb -c /home/sens/NICOLA/gnbLogs/armcfg/gnb_L1.yml" tests/passive_rx/ota/lab_sens4_run.sh <ARM>`.

- Il `ri=` nel log del gNB conferma che il cap ha effetto. Il gNB è validazione, non input.
- PRB: la cella è fissa a 273 PRB. Nessun arm PRB nelle fonti: da verificare.

---

## 6. Matrice degli esperimenti (adattata: 2 UE reali, 273 PRB, 4T4R, OCUDU)

Piano utente adattato: L1-L8 e metriche della sezione 8. Ogni run è un `lab_sens4_run.sh <ARM>` (o un `ARM` con variabili) salvo indicazione.

| id | esperimento | come si lancia | stato / note |
|---|---|---|---|
| L1 | baseline, 1 UE, traffico stabile | `PHONES="<un solo telefono>" ...` (vedi 4.1) con traffico DL/UL | da verificare: nessuna run 1-UE nelle fonti; con 2 telefoni è la baseline reale `[OTA MEASURED sens6]` |
| L2 | sweep del guadagno RX (al posto dell'SNR) | `RXG=49/42/36/30` alternati, `scripts/sweep.sh` (8 run) | fatto 2026-10-25: 36 ottimo, 49 clippa, 30 termico. Attenuazione fisica: non disponibile |
| L3 | MCS/TBS/rank (gNB arm) | `GNB_CMD=... -c armcfg/gnb_<L1\|L1M\|L2>.yml` (sezione 5) | nessuno sweep di PRB nelle fonti (da verificare) |
| L4 | multi-UE, 2 UE | run standard: OPPO + Samsung | risultati per RNTI (0x46xx) in `groundtruth` `per_rnti` |
| L5 | carico crescente fino a saturazione | `PHASE_BW="-b <rate>"` con più step | profilo di carico non definito nelle fonti: da verificare |
| L6 | 60 min di carico massimo | `DUR=3600 tests/.../lab_sens4_run.sh <ARM>` | ATTENZIONE: il pcap MAC gNB è ~8.5 GB ogni 5 min su sens4, cioè ~100 GB/h: controllare spazio su sens4 prima (da verificare). Il pcap non si disattiva senza cambiare `GNB_OVERRIDES` |
| L7 | cold start, 30-50 ripetizioni | N run identiche (`lab_sens4_run.sh <ARM>` ripetuto, con 60 s tra una e l'altra) | ogni run riavvia il gNB e il ricevitore: è un cold start. Tempo: DUR + ~40 s + avvio gNB, per run (da verificare) |
| L8 | riconfigurazione controllata | da verificare: metodo di riconfigurazione non nelle fonti | usare le metriche `CONFIG_EPOCH` e `DEDICATED_CHANGE_SUSPECTED` (attese ~0); piano: `docs/superpowers/plans/2026-10-01-reconfiguration-robustness.md` |

Note per L7 (metriche di cold start, da `run.log` / `summary.txt`):
- SSB/cella: `Cell Detected`, `Initial sync successful`, `SIB1 decoded`, `ACQ carrier` (`ACQ carrier CONFIRMED` a 273 PRB).
- PDCCH/CORESET: `multi-CORESET bank add` (atteso 1 su questa cella), `ULLAYOUT STATS bwp=LOCKED 273@0`.
- primo RNTI e primo PDSCH CRC OK: `PDSCHQ queued`, `EVMGRANT ... crc=1`; primo RNTI: da verificare il marker esatto.

---

## 7. Raccolta dati (layout delle directory)

```
/home/sens/NICOLA/captures/<ARM>_lab_<ts>/          # RUN (lab_sens4_run.sh)
├── receiver_launcher.log                            # stdout del launcher
├── lab_sens4_run.cfg                                # copia della configurazione
├── traffic_oppo.log  traffic_samsung.log            # fasi iperf3 DL/UL
├── gnb/
│   ├── gnb_s6lab_<ts>.stdout.log                    # log gNB (copiato da sens4)
│   ├── gnb_s6lab_<ts>.log                           # SCHED (mac_level info), copiato
│   ├── sched.csv  ocudu_gt.log                      # ground truth gNB (ocudu_gt.py)
├── plots/  plots.log                                # plots.py --lab
└── <ARM>_<ts>/                                      # run del ricevitore (launcher)
    ├── run.log                                      # log del ricevitore
    ├── summary.txt                                  # conteggi chiave + rc, nic_miss_delta, out_of_buffer_delta
    ├── nic.csv                                      # rx_missed_errors nel tempo
    ├── cmdline.txt                                  # CMD esatto + HEAD git
    ├── ue.passive...cfg, ota_lab273.s6.env          # copie
    ├── watchdog.txt                                 # solo se scattato STALL o secondo SIGINT
    └── export/                                      # export lab (tools/mobisys/export.py)
        ├── manifest.json
        ├── export.log
        └── groundtruth/  summary.json  matches.csv  ul_summary.json
```

- Il MAC pcap gNB (~8.5 GB / 5 min con mac_level info e entrambi i telefoni in iperf) resta su sens4: `/home/sens/NICOLA/gnbLogs/gnb_s6lab_<ts>.mac.pcap`.
- Il manifest di sessione richiesto (`session_manifest.json`: commit, config, HW, UHD, CUDA, frequenze, gain): non esiste come tale. Le fonti producono `manifest.json` (export) e `cmdline.txt` (CMD e HEAD). Il resto: da verificare.

---

## 8. Elaborazione

### 8.1 Ordine dei passi (post-run, ricevitore fermo)

Il `lab_sens4_run.sh` lo fa da solo alla fine (`ground_truth`). A mano, dal `$WT`, con `RUN` e `X` (= `<ARM>_<ts>/export`):

```
S="systemd-run --user --scope --quiet -p MemoryMax=1G --"
$S python3 -I tools/mobisys/ocudu_gt.py --log <RUN>/gnb/gnb_s6lab_<ts>.log --pci 2 --out <RUN>/gnb/sched.csv
$S python3 -I tools/mobisys/groundtruth.py --rx <X> --gnb-csv <RUN>/gnb/sched.csv --out <X>/groundtruth
$S python3 -I tools/mobisys/ul_groundtruth.py --rx <X> --gnb-csv <RUN>/gnb/sched.csv --rx-log <ARM>_<ts>/run.log --out <X>/groundtruth
$S python3 -I tools/mobisys/plots.py --lab <X> --out <RUN>/plots
```

- `groundtruth.py` accetta `--gnb-csv` (usato dallo script) oppure `--gnb-log` (grezzo, nel README). Il README dice `--gnb-log gnb.log` e "mac level debug": lo script usa il CSV da `ocudu_gt.py` con `mac_level=info`.
- Il cap a 3 GB del post-run in `lab_sens4_run.sh` è scelto per il post-run a ricevitore fermo. La regola "analisi con 1G" (sezione 9) vale con il ricevitore attivo.
- Test di unità: `cd tools/mobisys && python3 -m unittest -v` (33/33 riportati).

### 8.2 Metriche

| metrica | definizione | dove | nota |
|---|---|---|---|
| DCI recall | N_DCI_correct / N_DCI_transmitted | `groundtruth/summary.json` → `clock_matched.per_rnti.<dl/ul>.recall` (`match_clock`) | per RNTI; `groundtruth.py` stampa `groundtruth[clock]: ...`; valore da riportare: `gtc.sh` (orologio di parete) |
| DCI precision | tp / (tp + fp) | stesso file, `.precision` | report: 1.000 nel run finale `[OTA MEASURED sens6]` |
| TB recovery | N_TB_correct / N_TB_transmitted | `pdsch_crc`, `tb_ok`, `tb_fail` per DL | sui grant HARQ round-0: 98.2 % (rfsim, `[MEASURED, sens6 rfsim]`); su tutti i grant il valore è falsato dagli rv≠0 non risolti (artefatto del bed) |
| falsa accettazione | fp_dci | `summary.json` | candidato più vicino; definizione precisa: da verificare |
| errori di campo | — | — | da verificare: non misurati nelle fonti |
| PDSCH OK dato DCI corretto | per RNTI, `dci_ok_tb_failed`, `tb_ok` | `summary.json` (classi TB) | |
| UL DCI recall/precision | per RNTI | `ul_summary.json` (`tp`, `fp`, `fn`, `offset_slots`) | UL 0.51-0.99 per telefono tra le run: non stabile |
| PUSCH CRC UL | `UL_PUSCH_CRC` | `ul_summary.json` → `rx_pusch_crc` | |
| latenza p50/p95/p99/p99.9 | `e2e_tot_us` dei `rx_grants` | `plots/` (`latency_cdf`); percentili: da verificare | report: p50 1.41 ms, p99 13.7 ms (run dopo fix; non A/B) |
| code | `ISAC_METRICS`: `scanq_queued`, `scanq_drop_full` | `run.log` | drop atteso 0-2 % (K45) |
| CPU/RSS | — | `plots/` (`cpu_rss`) | |
| GPU | `GPU_PATH choice=`, `GPU_MEM` | `run.log` | |
| IQ overflow/drop | `nic_miss_delta`, `out_of_buffer_delta` | `summary.txt`, `nic.csv` | attesi 0 |
| SFO | `SFO`, `SFO_DMRS` | `run.log` | eps ~ +3.8 ppm `[OTA MEASURED sens6]` |
| clipping | `ADCCLASS` | `run.log` | per classe slot DL/UL/speciale |

Metriche di coerenza (attese):
- `multi-CORESET bank add` = 1 su questa cella.
- `CONFIG_EPOCH ... DEDICATED_CHANGE_SUSPECTED` ~ 0.
- `ULLAYOUT STATS bwp=LOCKED 273@0`.
- `ISAC_RX_BRANCH_FO` non impostato (regola rigida).

### 8.3 Sincronizzazione

- Chiave di match: `(frame mod 1024, slot, rnti)` dentro una finestra di wall clock di 2.0 s (`match_clock`), dopo aver tolto l'offset modale di orologio (bin da 5 ms). Il periodo SFN di 10.24 s rende la chiave unica nella finestra; lo SFN wrap è gestito così.
- Il vecchio matcher con un solo offset (`match()`) si rompe a ogni recupero del ricevitore: il run 02:11 dava 0.62/0.71 invece di 0.80/0.9995 `[OTA MEASURED sens6]`. Usare sempre `match_clock`.
- Il `ul_groundtruth.py` allinea sulla slot del DCI (`dci_frame/dci_slot` = slot PUSCH − k2).

### 8.4 Script di analisi

Cartella: `docs/superpowers/experiments/scripts/` (copiati dallo scratchpad il 2026-10-25). Eseguire l'analisi a ricevitore fermo, o in `systemd-run --user --scope -p MemoryMax=1G`.

| script | cosa fa | uso |
|---|---|---|
| `an.py` | per run: DL/UL per larghezza del grant (1-4, 5-32, 33-128, >128 RB; UL 1-8, 9-64, >64) + censimento ADC; streaming, poca RAM | `python3 -I an.py <RUN> [<RUN> ...]` |
| `layers.py` | per run, grant rank 4 non-probe: SNR per layer (`snr_l`) e CRC per larghezza | `python3 -I layers.py <RUN>...` |
| `slotpos.py` | CRC per posizione nel frame (slot % 40) per larghezze 5-32 e 33-128 RB, rank 4 | `python3 -I slotpos.py <RUN>...` |
| `dmrsnr.py` | SNR DM-RS (`dmrs_snr`) e varianza del rumore (`nvar_db`) per grant 5-32 RB, rank 4 | `python3 -I dmrsnr.py <RUN>...` |
| `disc_score.sh` | per run: bank add, `evidence ignored`, `PDCCH_NID_GPU pass`, % `scanq_drop_full`, SCANTIME, PUSCH try | `./disc_score.sh <RUN>...` (legge `*/run.log`) |
| `sweep.sh` | sweep del guadagno (49 42 36 30 30 36 42 49) con `lab_sens4_run.sh`; ABORT se l'IP NIC manca o `MemAvailable` < 7 GB | `./sweep.sh` (scrive `sweep.log`) |
| `integ.sh` | serie di integrazione: A/B UL poi arm DL di rank/MCS con `RXG=36` e `GNB_CMD` per arm; ABORT su NIC o memoria o `launcher rc=3` | `./integ.sh` (scrive `integ.log`) |
| `mcsw.py` | DL CRC per numero di layer × larghezza × fascia MCS | `python3 -I mcsw.py <RUN>...` |
| `l2snr.py` | rank 2 (MCS ≥ 24): SNR minimo per layer, gap tra layer, SNR DM-RS vs CRC | `python3 -I l2snr.py <RUN>...` |
| `ulpos.py` | UL CRC per larghezza del grant × posizione nella banda (basso / medio / alto) | `python3 -I ulpos.py <RUN>...` |
| `ulq.sh` | verdetto UL per run: percorso di lock del layout (FIT_STRICT / SEED_LOCK / ULLAYOUT_CRC_LOCK), stato finale bwp, UL CRC per larghezza | `./ulq.sh <RUN>...` |
| `gtc.sh` | DCI recall/precisione per RNTI a orologio di parete (`groundtruth.py --rx <run>/*/export --gnb-csv <run>/gnb/sched.csv`) | `./gtc.sh <NOME_RUN>...` (cerca in `captures/`) |
| `score6.sh` | combina `mcsw.py` + `an.py` + `disc_score.sh` | `./score6.sh <RUN>...` |

- `ulq.sh`, `gtc.sh`, `score6.sh` usano percorsi fissi: `gtc.sh` usa `agn-wt/hmcs` e `/home/sens/NICOLA/captures/`; `ulq.sh` e `score6.sh` richiamano `mcsw.py`/`an.py`/`disc_score.sh` dalla cartella scratchpad `gain`. Verificare e correggere i percorsi prima dell'uso.
- `gtc.sh` è l'unico recall affidabile. La riga `groundtruth:` dell'output lab usa un solo offset e sbaglia dopo i salti di slot del ricevitore `[OTA MEASURED sens6]`.

- I log `*.out` nella stessa cartella sono output di run già fatti (`g49.*.out`, `ab_*.out`, …).
- `sweep.sh` e `integ.sh` hanno percorsi di worktree/build fissi (2026-10-25): modificarli prima dell'uso.

### 8.5 Log da controllare per run (2026-10-26)

`[OTA MEASURED sens6, cella lab]`
- `ULLAYOUT FIT_STRICT`, `ULLAYOUT SEED_LOCK bwp=273@0`, `ULLAYOUT_CRC_LOCK len=43 bwp=273@0`: percorso di lock del layout UL.
- `ULLAYOUT STATS bwp=LOCKED 273@0` finale: se resta `searching`, UL persa per quella run.
- `UL DATA_ID LOCK rnti=... n_id=2`: data-id UL agganciato.
- `DCI 1_1 length locked ... len=47`: le altre lunghezze sono spurie.
- `SENSING: GPU_PATH ... choice=`: il percorso di decode è scelto per run da un trial di tempo.

### 8.6 Risultati misurati (riepilogo)

`[MEASURED, sens6 OTA, sens4 lab cell, gain 36]`
- DCI DL recall per telefono 0.97-0.99, precisione 1.0 (scorer a orologio di parete).
- PDSCH CRC DL a MCS 24-27, dopo i fix:

| rank | ≤32 RB | 33-128 RB | >128 RB |
|---|---|---|---|
| 1 | 0.66-0.79 | 0.76-0.78 | 0.61 |
| 2 | 0.30-0.63 | 0.30-0.65 | 0.10-0.47 |

- Rank 2: forte variazione tra run a pari SNR, causa ignota `[HYPOTHESIS]`.
- Rank 4, MCS 20-23: ≤32 RB circa 0.52-0.57; wide circa 0.05-0.15.
- Rank 4, MCS 24-27: narrow circa 0.1; wide circa 0. Compatibile con il condizionamento 4x4 alla posizione del ricevitore `[HYPOTHESIS, non dimostrato]`.
- UL: layout lock in 4/4 run dopo `c0736a228e`. UL CRC ≤8 RB 0.81-0.97; >64 RB 0.36-0.96 (varia per run).
- Fix `[CODE-READ]` (una riga ciascuno):
  - equalizzatore DL rank 2, modalità exact (`cac200ad51`)
  - normalizzatore target LLR 256QAM (`2bb691c386`)
  - correzione SFO in-slot, DL e UL
  - arbitro di re-espressione discovery CORESET
  - tempesta di riconfigurazioni
  - lock UL falsificabili dal CRC
  - lunghezza DCI della cella per corroborazione (`0e4028574c`)
  - ordine data-id UL con l'id confermato dal DL per primo
  - selezione run-time del ramo UL
  - fit del layout UL (`c0736a228e`)
- Regola: almeno 2 run per braccio, alternate. La variabilità run-to-run è ampia.

---

## 9. Problemi hardware e soluzioni

| sintomo | causa | soluzione |
|---|---|---|
| X410 non parte: `RFNoC ctrl ACK timeout` / `rpc::timeout get_db_eeprom` | IRQ NIC su CPU 10-13 (lane RT) | IRQ NIC solo su CPU 0 (housekeeping) `[OTA MEASURED sens6, 6 su 7 start falliti]` |
| LMX2572 non risponde all'init | sintetizzatore di clock X410 | un solo retry dell'avvio (riportato in OTA-DCI-LAB273 §open 3) |
| `[ERROR] [LMX2572] Unable to communicate with LMX2572` all'init X410, launcher rc=1 | sintetizzatore di clock X410 | rilanciare la run. Accaduto 3 volte (2026-10-25/26: r4new2, u8D2, una precedente) `[OTA MEASURED sens6]`. Le run subito dopo questo errore possono essere degradate `[HYPOTHESIS, non verificata]` |
| "could not take the testbed lock" subito dopo una run | lock testbed ancora in uso dal teardown | attendere e rilanciare la run |
| `RFSTALL` / errore metadata UHD (`USRP_RX_READ`) | perdita di pacchetti sul NIC | controllare `nic_miss_delta` e `out_of_buffer_delta` in `summary.txt`, poi rilanciare. `USRP_RX_READ` non è nelle fonti: da verificare |
| "Someone tried to claim this device again" | run precedente fermata, X410 ancora reclamato | attendere ≥ 60 s dopo una run fermata; controllare `claimed: False` nel `check` |
| indirizzo NIC perso (entrambi rimossi da altri, 2026-10-25) | reset esterno | `sudo ip addr add 192.168.20.65/24 dev enp2s0f1np1`; controllare prima di ogni run |
| ADC clipping (`ADCPEAK` a -12.0 dBFS = fondo scala X4xx) | il driver sposta i campioni di 2 bit (rxshift=2, fs 8191) | ridurre `RXG` a 36 (49 clippa in DL) |
| telefono non agganciato | modo aereo / rete | toggle automatico dello script; verificare `adb devices` su sens4 / sensnuc3 |
| lock testbed bloccato dopo crash | tmux `s6lab_*` ancora vivi | `tmux send-keys -t s6lab_<x> C-c` per le sessioni; riportare la nota di lock a `stop_all ...` (idle) |
| traffico che sopravvive al teardown e tiene `td_measure.lock` | bug corretto | commit `7f235a8fbf` in `lab_sens4_run.sh`; verificare `ps -eo pid,cmd \| grep "[l]ab_sens4_run"` |
| preflight "source newer than the binary" (agente che edita il worktree) | sorgenti più nuovi del binario | usare la copia congelata (sezione 4.0); mai `FORCE=1` per risultati |
| OOM | memoria | regole di memoria (sezione 10); un OOM ha spento l'host il 2026-10-25 |

Limiti noti del ricevitore (misurati, cella sens4):
- Grant DL rank-4 larghi (>64 RB) limitati dal condizionamento 4x4 alla posizione del ricevitore. Il rumore è proporzionale al segnale: SNR DM-RS ~30 dB costante tra gain 42 e 36 `[OTA MEASURED sens6, sweep 2026-10-25, scripts/dmrsnr.py]`, cioè floor ~-30 dB. Sorgente (EVM TX gNB/O-RU vs rumore di fase LO X410) non separabile con il guadagno `[HYPOTHESIS: TX EVM]`.
- Fix CSI-RS slot in corso (`sens6/csirs-cdm`).
- Rank-4 a 256QAM MCS 24-25: CRC su un "precipizio" (SNR pass 24.8 vs fail 23.8 dB) `[OTA MEASURED sens6]`.

---

## 10. Checklist pre-run

- [ ] `pgrep -x nr-uesoftmodem` vuoto (nessun ricevitore, nessuna build)
- [ ] nessuna analisi pesante in corso su sens6
- [ ] `MemAvailable` ≥ 7 GB (`grep MemAvailable /proc/meminfo`)
- [ ] `ip -4 -br addr show enp2s0f1np1` mostra `192.168.20.65/24`
- [ ] IRQ NIC su CPU 0; sysctl applicati (dopo reboot)
- [ ] `run_ota_s6.sh check` → 0 fatal, 0 warnings
- [ ] binario: `$WT/build_ota/nr-uesoftmodem` con tutti i plugin; nessun sorgente più nuovo
- [ ] `ISAC_RX_BRANCH_FO` non impostato (`env | grep ISAC_RX_BRANCH_FO` vuoto)
- [ ] sens4: lock testbed in `stop_all ...`, nessuna sessione `s6lab_*`, nessun `gnb`
- [ ] `adb devices` mostra `ff1ecb98` (sens4) e `R5CX11CBYJP` (sensnuc3)
- [ ] `--dry-run` eseguito e letto
- [ ] spazio disco su sens4 per il pcap (~8.5 GB / 5 min)
- [ ] ≥ 60 s dall'ultima run fermata (X410 non reclamato)
- [ ] ARM e `RXG`/`DUR` scritti nel log del lavoro
- [ ] per un risultato di gate: ≥ 5 run per arm alternate, con verdetto di validità per run (PROJECT_MEMORY §26)
- [ ] almeno 2 run per braccio, alternate (la variabilità run-to-run è ampia)
- [ ] log per run controllati (sezione 8.5)
- [ ] mai `FORCE=1`, mai SIGKILL, mai `pkill -f`
- [ ] nessun commit di dati o pcap nel repository

---

## Riferimenti

- `tests/passive_rx/ota/lab_sens4_run.sh`, `.cfg`
- `tests/passive_rx/sens6_ota/run_ota_s6.sh`, `ota_lab273.s6.env`, `ue.passive.ota.lab273.s6.cfg`
- `tools/mobisys/README.md`, `ocudu_gt.py`, `groundtruth.py`, `ul_groundtruth.py`, `plots.py`
- `docs/superpowers/reports/csd/OTA-READY-20261024.md`, `OTA-DCI-LAB273-20261025.md`, `OTA-SNR-WIDTH-LAB273-20261025.md`, `WIDE-RANK4-20261025.md`
- `PROJECT_MEMORY.md` §15.00, §26
