# Passive-RX Operations

Everything runs on **sens6** (X410 host). The gNB is on **sens4**.
Paths below are sens6 paths. Written 2026-09-13; commands verified against
`tests/passive_rx/captures/run_arm.sh` and `tests/passive_rx/monitor/monitor.py` as they exist today.

| thing | where |
|---|---|
| repo | `/home/sens/NICOLA/adaptive-rx-UL-DL` |
| old (pre-UL) repo | `/home/sens/NICOLA/adaptive-rx-sensing` |
| binary | `<repo>/cmake_targets/ran_build/build/nr-uesoftmodem` |
| capture harness | `<repo>/tests/passive_rx/captures/run_arm.sh` |
| dashboard | `<repo>/tests/passive_rx/monitor/monitor.py` |
| captures land in | `/home/sens/NICOLA/captures/<ARM>_<HHMMSS>/` |
| X410 mgmt / data | `128.178.122.174` / `192.168.20.2` (host NIC `enp129s0f0np0`, `192.168.20.1/24`) |
| gNB config / log | sens4: `/home/sens/gnb.yaml`, `/home/sens/NICOLA/gnbLogs/gnb.log` |

---

## 0. Two rules that have cost whole afternoons

**Never build while a capture is running.** 8 compile jobs starve the receiver's RT thread and the
capture silently becomes garbage. Check first:

```bash
pgrep -cx nr-uesoftmodem     # must be 0 before you build
```

**Never put a `pkill -f <pattern>` on an ssh command line.** The pattern matches your own ssh
session and kills it. Put patterns in a script file, or bracket them:

```bash
pkill -f 'run_arm[.]sh'      # bracket form: the literal text no longer matches itself
```

Related: `run_arm.sh` aborts with `PREFLIGHT ABORT: source newer than binary` if you edited source
without rebuilding. If you must edit mid-campaign, park the change:

```bash
cd /home/sens/NICOLA/adaptive-rx-UL-DL
git diff <file> > /tmp/my.patch && git checkout <file>
B=cmake_targets/ran_build/build/nr-uesoftmodem
touch -d "@$(( $(stat -c %Y $B) - 3600 ))" <file>    # restore an older mtime so preflight passes
```

---

## 1. Run a capture (normal, agnostic mode)

`run_arm.sh` is entirely environment-driven. It retries until the run is VALID.

```bash
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/captures

REPO=/home/sens/NICOLA/adaptive-rx-UL-DL \
CONF=/home/sens/NICOLA/captures/agnostic_ota.conf \
ARM=mytest DUR=600 TRIES=3 SCAN=1 NANT=4 RXG=40 \
./run_arm.sh
```

Result goes to `/home/sens/NICOLA/captures/mytest_<HHMMSS>/run.log` plus `verdict.txt`.

### The knobs you will actually use

| var | default | meaning |
|---|---|---|
| `ARM` | — | name prefix for the capture directory |
| `DUR` | 200 | capture seconds |
| `TRIES` | 3 | retries until VALID |
| `SETTLE_S` | 180 | wait after a `usrp-hwd` restart — **do not shorten**, see §4 |
| `PRB` / `CARRIER` | 273 / 3450000000 | cell bandwidth and centre frequency |
| `SSB` | 150 | SSB subcarrier offset (used when `SCAN=0`) |
| `SCAN` | 0 | 1 = `--ue-scan-carrier` (blind), 0 = use `SSB` |
| `NANT` | 4 | RX/TX antennas |
| `RXG` | 40 | RX gain |
| `INITIALFO` | -15000 | CFO seed, Hz, same sign as the reported offset |
| `AUTOACQ` | 0 | 1 = integer-SCS ambiguity search at acquisition |
| `CPUSET` | 0-7 | cores for the process |

### Diagnostic probes (set to 1 to enable; all default off)

| var | what it turns on |
|---|---|
| `EVMPROBE` | `EQDIAG` — post-equaliser EVM + \|H\| per frequency bin. **The main DL diagnostic.** |
| `SFOCORR` | residual SFO correction across subcarriers |
| `FULLCRC` | full PDCCH CRC reporting (only way to see real DCIs vs accepts) |
| `LLRPROBE` / `LLRSCALE` | LLR inspection / scaling |
| `CFGSWEEP` | PDSCH config hypothesis sweep |
| `DCIGT` | DCI ground-truth cross-check |
| `MRC` | RX combining mode |
| `PUSCHDIAG` / `ULPROBE` | uplink decode diagnostics |

Full list: `grep -oE '\$\{[A-Z_]+:\+' run_arm.sh | sort -u`

---

## 2. Manual mode (pin the cell layout instead of deriving it)

Agnostic mode derives CORESET/BWP/TDA/DM-RS from the air. When it derives them wrong, DL decode
collapses and it looks like an RF problem. Manual mode removes that variable — use it as the
control whenever DL decode is bad.

A known-good manual conf lives in every old capture, e.g.
`/home/sens/NICOLA/captures/sensing_manual_fixed.3NB5Ri/receiver.conf`:

```
pdcch_blind_monitor_coreset = "45:1:0:0:0:2";   // num_groups:duration:reg_bundle:interleaver:shift:scramb
pdcch_blind_monitor_bwp     = "0:273:0:47";     // BWPStart:BWPSize:refPoint:dci_length
pdcch_blind_monitor_tda     = "1:13,1:7";       // startSymbol:length, one per TDRA entry
pdcch_blind_monitor_dmrs    = "2:1";            // dmrs symbol position : additional position
pdcch_blind_monitor_ul_bwp  = "0:273";
pdcch_blind_monitor_ul_tda  = "0:14:4:0,0:14:5:0,...";
```

Run it directly (this is the exact invocation that historically gave ~92 % CRC on 260-PRB grants):

```bash
OUT=/home/sens/NICOLA/captures/manual_$(date +%H%M%S); mkdir -p $OUT
cp /home/sens/NICOLA/captures/sensing_manual_fixed.3NB5Ri/receiver.conf $OUT/
cd $OUT
sudo env ISAC_PDSCH_EVM=1 setsid nohup bash -c "ulimit -c 0; exec timeout 600 \
/home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build/nr-uesoftmodem \
--usrp-args type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.174 \
-O $OUT/receiver.conf -r 273 --numerology 1 --band 78 -C 3450000000 --ssb 150 \
--ue-rxgain 40 --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation \
--cont-fo-comp 1 --freq-sync-P 0.05 --freq-sync-I 0.001 --initial-fo -16480 \
--thread-pool 0,1,6,7 --time-sync-I 0.01 --ntn-initial-time-drift -4.25 -A 90" \
> $OUT/run.log 2>&1 &
```

**No `#` comment may appear anywhere inside that `bash -c` string** — a `#` truncates argv and
silently drops every later flag. This has broken `--thread-pool` twice, which looks exactly like a
UHD overflow bug.

To check whether the layout was derived correctly in an agnostic run, compare against the manual
values above:

```bash
grep -a 'SENSING: PARMSET' $OUT/run.log | tail -16
```

Many mutually-contradictory `sym=`/`dmrsmask=` values means discovery has not converged.

---

## 3. Dashboard

On sens6:

```bash
cd /home/sens/NICOLA/adaptive-rx-UL-DL/tests/passive_rx/monitor
./monitor.py --connect tcp://127.0.0.1:5556 --port 8080 \
             --log $(ls -dt /home/sens/NICOLA/captures/*/ | head -1)/run.log
```

Then from your machine:

```bash
ssh -N -L 8080:localhost:8080 sens6      # keep open
```

and open <http://localhost:8080>.

- `--connect` may be repeated for several receivers (shown side by side, never fused).
- `--log` makes it tail radio/decode health as well as detections; it re-resolves to the newest
  `captures/*/run.log` on its own, so it survives a capture restart.
- `--bind 0.0.0.0` is the default, so you can also hit `http://<sens6>:8080` directly.
- `--seed FILE.jsonl` / `--replay FILE.jsonl` to view a finished run offline.

"No receiver connected" means nothing is publishing on 5556 — check the receiver is running and
that `[sensing] report_endpoint` in the conf matches.

---

## 4. X410 faults — identify and fix

### 4.1 Stale MPM claim (most common)

**Symptom:** the receiver dies at startup, or UHD refuses to open the device.
**Cause:** a second process tried to claim the X410; MPM kills *itself* and is down ~100 s. A
SIGKILLed softmodem leaves the claim behind.

**Never open a second UHD session against the X410 while a capture runs** — not even a one-line
`uhd.usrp.MultiUSRP()` probe. That *is* the second claim.

```bash
# see it (only visible in MPM's own journal)
ssh root@128.178.122.174 'journalctl -u usrp-hwd -n 50 --no-pager' | grep -iE 'claim|Terminating|Stopping'
# fix
ssh root@128.178.122.174 'systemctl restart usrp-hwd'
sleep 180          # SETTLE. Not optional -- see 4.2
```

### 4.2 Too-short settle after an MPM restart

**Symptom:** long streaks of "PSS found, PBCH never decodes". Looks exactly like a code regression.
**Cause:** the front end needs ~180 s to settle; `ANTPOW` sits near the noise floor before that.
**Fix:** `SETTLE_S=180` minimum (harness default). Verify recovery before trusting a run:

```bash
grep -a ANTPOW $OUT/run.log | tail -1
# healthy: raw=[~200 ~100 ~500 ~400]. If every branch reads ~10, it has not settled.
```

### 4.3 Deaf stream / RX overflow (`OOOO`)

**Symptom:** the process is alive, the log stops growing (or only prints `OOOO`), no new decodes.
**Cause:** one RFNoC RX overflow permanently halts the stream.
**A live process is not a working receiver** — never infer liveness from `pgrep`.

```bash
tail -c 200 $OUT/run.log                 # trailing OOOO = overflowed
d=$OUT/run.log; s1=$(stat -c %s $d); sleep 20; s2=$(stat -c %s $d)
[ "$s1" = "$s2" ] && echo "STALLED"
```

Fix: kill, restart `usrp-hwd`, settle 180 s, rerun. Reduce load (fewer antennas, fewer probes) if
it recurs immediately.

### 4.4 Host NIC starvation (drops without an X410 fault)

**Symptom:** UHD timeouts / overflows that track `rx_missed_errors` climbing.
**Cause:** MTU or ring size reset (not reboot-persistent), or softirq starvation.
**Always read the DELTA across the run, never the absolute counter.**

```bash
NIC=enp129s0f0np0
cat /sys/class/net/$NIC/mtu                       # must be 9000
ethtool -g $NIC | awk '/Current hardware/,0'      # RX/TX must be 8192
ip addr show $NIC | grep 192.168.20.1             # must be present
# restore
sudo ip link set $NIC mtu 9000
sudo ethtool -G $NIC rx 8192 tx 8192
sudo ip addr add 192.168.20.1/24 dev $NIC
# delta across a run
cat /sys/class/net/$NIC/statistics/rx_missed_errors   # before and after
```

`run_arm.sh` checks and restores all three automatically in its preflight.

### 4.5 Timing-loop runaway (RFSTALL)

**Symptom:** stream dies mid-run; `shiftForNextFrame` swinging ±21, `max_pos_acc` ±2000.

```bash
grep -aE 'RFSTALL|shiftForNextFrame|max_pos_acc' $OUT/run.log | tail -20
```

Not caused by grant rate or RF overload (both tested and refuted). Restart the run.

### 4.6 CFO mis-lock

**Symptom:** exactly 0 % DL CRC with otherwise healthy signal. CFO is estimated **once** at
acquisition; if it locks badly the whole run is void.

```bash
grep -a 'Measured Carrier Frequency offset' $OUT/run.log
grep -a 'DMRSFO cfo=' $OUT/run.log | tail -3     # residual; |cfo| < 300 Hz is healthy, > 2600 Hz is dead
```

**Abort and retry — do not retune a live radio.** A mid-run retune killed the radio 2/2 times.
Adjust `INITIALFO` and restart instead.

---

## 5. Reading a capture

### Verdict (written by the harness)

```bash
cat $OUT/verdict.txt
```

| verdict | meaning |
|---|---|
| `VALID` | usable |
| `VOID_CFO_MISLOCK` | bad CFO lock — §4.6 |
| `VOID_NO_SIB1` | never decoded SIB1 |
| `VOID_DL_ZERO` | no DL LDPC successes |
| `VOID_NO_CPI` | no CPI closed (**vacuous when sensing is off** — ignore for DL-only runs) |
| `VOID_DL_RATE` | DL TB rate under 15 % |

**Score a run before reading its numbers.** Roughly half of all captures are void.

### The lines worth grepping

```bash
grep -a 'SENSING: PDSCHQ queued'  $OUT/run.log | tail -1   # DL CRC, queue lag, stale drops
grep -a 'SENSING: RBHIST'         $OUT/run.log | tail -1   # CRC as a function of allocation width
grep -a 'SENSING: SHAPE'          $OUT/run.log | tail -3   # decoded vs failed grant geometry
grep -a 'SENSING: EQDIAG'         $OUT/run.log | tail -3   # EVM + |H| per frequency bin (EVMPROBE=1)
grep -a 'SENSING: ANTPOW'         $OUT/run.log | tail -1   # per-branch RX power
grep -a 'SENSING: PARMSET'        $OUT/run.log | tail -16  # derived PDSCH layouts
grep -a 'SENSING: ACQ_STATE'      $OUT/run.log            # acquisition state machine
grep -a 'SENSING: DMRSFO'         $OUT/run.log | tail -3   # residual CFO / SFO
grep -a 'Cell Detected'           $OUT/run.log | tail -1   # did it acquire at all
```

**`RBHIST` is the highest-value single line for DL problems**: a smooth roll-off with allocation
width is link margin; a cliff is a bug.

### Counter traps

- `pdsch_decode[try=... crc_ok=...]` reads **0** when DL goes through the queue. Use `PDSCHQ`.
- `LDPCDIAG ok=` counts empty transport blocks too. Score `segs_decoded` / `dl_tb%`.
- ~30 % of grants carry no MAC PDU (gNB sends PDSCH with an empty TB), so `crc_ok/try` is not
  decoder health on its own.

---

## 6. gNB side (sens4)

Always resolve config and log **from the running process** — both move, and a stale log reads as
"no traffic".

```bash
ssh sens4 'P=$(pgrep -x gnb|head -1); tr "\0" "\n" < /proc/$P/cmdline | grep -A1 -- -c | tail -1'
ssh sens4 'ls -l /proc/$(pgrep -x gnb|head -1)/fd | grep -oE "/[^ ]*\.log"'
ssh sens4 'grep -E "max_rank|max_ue_mcs|dl_arfcn|pci|nof_antennas" /home/sens/gnb.yaml'
```

Notes:
- `dci_aggregation_level=N` in the gNB log means **AL = 2^N**, not N.
- Grep PDSCH counters with `PDSCHs\?` — a plural-only pattern drops every single-PDSCH slot and
  produces a false "zero DL traffic".
- `OFH ... woke up late, skipped N symbols` warnings are **chronic** on this rig and present in
  configs that decode fine. Never attribute a failure to them.
- Allocation width is driven by offered load: with no traffic the scheduler issues ~2-PRB grants and
  wide-grant behaviour is untestable. A heavily loaded cell, conversely, makes PBCH acquisition
  harder — expect retries.

---

## 7. Quick triage

```
DL CRC bad?
 ├─ verdict.txt VOID?                     -> fix that first (§5)
 ├─ ANTPOW all ~10?                       -> front end not settled (§4.2)
 ├─ log stopped / trailing OOOO?          -> deaf stream (§4.3)
 ├─ DMRSFO |cfo| > 2600 Hz?               -> CFO mis-lock, retry (§4.6)
 ├─ RBHIST cliff vs width?                -> receiver bug; run EVMPROBE=1 and read EQDIAG
 ├─ RBHIST smooth roll-off?               -> link margin; check gNB max_ue_mcs
 └─ PARMSET shows many conflicting sym/dmrsmask?
                                          -> layout discovery not converged; retest in manual mode (§2)

Suspect a regression ("it worked before")?
 -> run the OLD binary (/home/sens/NICOLA/adaptive-rx-sensing) with the SAME conf and flags,
    same evening, same cell. Ten minutes, and it separates "our code broke" from "the rig changed".
    Do this FIRST, before any instrumentation.
```
