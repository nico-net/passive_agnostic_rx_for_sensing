# X410 Passive Blind-PDCCH — Handover (2026-08-20)

**Read §1 (Rules) before touching anything.** This session produced three retractions, all from the
same class of mistake. The rules exist so the next agent does not repeat them.

Machine: **sens6** (the X410 host — sens3 is retired). Tree:
`/home/sens/NICOLA/openairinterface5g-total-passive-ue`, branch `nrsniffer-pdcch-pdsch-migration`.

---

## 1. Rules to respect

**R1 — A spiked statistic is not a decode.** Before claiming a recovered DCI, require ALL THREE:
  1. the full 24-bit CRC has its **upper 8 bits zero** (`upper=0x0`) — `rnti` in the result struct is
     `(uint16_t)crc` and hides them;
  2. the decoded **payload VARIES between instances** (real scheduling changes RIV/MCS/HARQ/NDI
     every grant — an invariant payload is a decoder fixed point, not traffic);
  3. the candidate **survives the energy gate** (a real signal does not vanish when a NOISE gate is
     switched on).
  Violating (2) and (3) is exactly how `0x5199` was mistaken for a C-RNTI and a false root cause was
  reported. `upper==0` alone is only a 1-in-256 test: on 10 M candidates ~42 k pass BY CHANCE.

**R2 — Take the RNTI from the gNB log, never from a quoted value.** It churns on every re-attach.
  Values quoted to me (`0x463b`, `0x4616`) did not match the log (`0x4627`, `0x4604`) three times.
  Better still: self-detect it (see §6 stage 1).

**R3 — Bracket every capture with an air-power check, before AND after.** The cell went down
  unannounced 3x in one session; two captures were scored on a dead stream before this was adopted.
  Recipe in §6. A live gNB is not a transmitting gNB.

**R4 — Re-apply the NIC tuning after every reboot.** It is NOT persistent, and sens6 rebooted
  mid-session. Without it the 100 GbE link silently drops samples. See §2.

**R5 — Never `pkill -f nr-uesoftmodem` over SSH** — it matches the SSH command line and kills your
  session. Use `pkill -9 -x nr-uesoftmodem`. For scripts, use a bracket pattern (`dci_swee[p].sh`)
  so the pattern cannot match itself.

**R6 — Check what your grep actually matches.** Two real errors this session: `grep -c "a|b|c"`
  without `-E` (matched nothing, reported "0 overflows" while testing nothing) and grepping for
  `"polar decoding wrong"`, a string this tree never emits. Sanity-check every zero.

**R7 — Do not run heavy logging during a capture.** Full-rate probe output breaks the attach that
  provides the control arm, and a concurrent build starves the UEs. All probes take a slot filter
  for this reason.

**R8 — Read `CLAUDE.md`'s pending section and the memory files BEFORE hypothesising.** I burned
  ~10 minutes re-running a `dci_length` sweep that the 2026-08-04 record explicitly says not to run.

**R9 — Judge decode success by the CRC probe, NOT by the `accepts` counter.** Everything after the
  CRC parses the payload as **DCI 1_1** and rejects on format-indicator / antenna-ports /
  reserved-MCS. SIB1 is format **1_0**, so 438 genuine decodes were reported as `accepts=1`.

---

## 2. Environment

- X410: serial `327C1F2`, data `192.168.20.2` over `enp129s0f1np1` (100 GbE), mgmt `128.178.122.3`.
  UHD 4.10. sens6 itself is `128.178.122.20` / tailscale `100.65.157.61`.
- **NIC tuning (re-apply after every reboot):**
  ```bash
  IF=enp129s0f1np1
  sudo ip link set dev $IF mtu 9000
  sudo ethtool -G $IF rx 8192 tx 8192
  sudo sysctl -w net.core.netdev_max_backlog=250000 net.core.rmem_default=62500000 \
                 net.core.rmem_max=250000000 net.core.wmem_max=250000000
  ping -c1 -M do -s 8972 192.168.20.2   # must succeed
  ```
- **No RF overflows on sens6** — `benchmark_rate` at 4x122.88 MS/s gives 0 drops/0 overruns, and all
  captures show 0 overflow / 0 timeout / 0 short-read. The sens3 overflow root cause does NOT
  reproduce here. `dropped=` lines in UE logs are the sensing engine's CFR queue, not RF.
- **sens6 has no AVX-512.** Any tree copied from sens3 must be clean-rebuilt
  (`cd cmake_targets && ./build_oai --nrUE -w USRP -c`). Generator is **Unix Makefiles**, not ninja.
- **`ISAC_DISC_NO_RESYNC=1` is REQUIRED** for any attached-UE run on this tree, or it re-acquires
  endlessly and never attaches (measured: sync 74 -> 4, RA 0 -> 1).
- Known intermittent **segfault in initial sync** (~1 in 18 starts). It dies at `CHRMS acquisition`,
  before any decode. Do not misattribute it to whatever you were testing — just re-run.

## 3. Cell / config ground truth (verified against the gNB's own debug log)

srsRAN 4T4R, `dl_arfcn 627666` = 3414.99 MHz, 100 MHz, SCS 30, PCI 2, `max_rank: 1`, SSB 20 ms.

| | dedicated | CORESET0 (SIB1) |
|---|---|---|
| BWP | `[0..273)` | `[1..49)` = 48 RB |
| freq domain | 45 groups = 270 RB | 8 groups |
| mapping | NON interleaved | interleaved `bundle6 / ilv2 / shift2` |
| DM-RS scrambling | `nid_pdcch_dmrs=2`, `nrnti=0` | same |
| aggregation | **AL1** (4406/4407) | AL4 |
| payload | 47 (DCI 1_1), 43 (0_1) | 39 |
| CCE index | spread 0..44 | 0 |
| slot index | 0-7 and 10-17, peaks at 4,5,14,15 | always **1**, every 16th SFN |

**Our config matches all of this exactly. Stop re-deriving it.** Note srsRAN's
`dci_aggregation_level` is very likely **log2(L)** — SIB1 logs `=2` while SearchSpace#0 and RRC both
say AL4. Our ladder covers 1/2/4/8 either way.

## 4. Established (trustworthy, measured)

- **The blind PDCCH chain WORKS on CORESET0**: 438 genuine SIB1 decodes (`crc=0xffff`, `upper=0x0`),
  and for matched `(frame, slot, cce, L)` the blind and normal paths produce **byte-identical LLRs**
  (`sum`, `nz`, first 8 soft bits). Extraction, channel estimation, unscrambling, polar decode and
  CRC/RNTI recovery are all correct.
- The normal path also recovers the RNTI from the CRC with the identical `polar_decoder_int16(...)`
  call and identical candidate stride — the blind decode is NOT a "weaker" operation.
- **Dedicated grants decode ~never** (0-1 per 100 s across four real C-RNTIs) while SIB1 decodes
  438x in the same binary, same run.
- **Energy IS present**: strongest-candidate/noise-floor ratio p99 = 7.65, max 20.47. So "nothing to
  decode" is excluded — this is a pointing/alignment problem, not link budget.
- **Ground-truth slot correlation (the key open clue):** the gNB's busiest dedicated slots (14/15,
  493 and 703 grants) show **flat** energy at our receiver (ratio 1.18/1.19), while slots 4/5 (414
  and 429 grants) do show elevation (1.72/1.86). Same CORESET, same run.
- `ncand=64` is **saturated** on the dedicated CORESET: AL1 fills 45, AL2 takes 19 (CCE 0..36), and
  the AL4/AL8 loops never execute there. Not fatal (grants are AL1, fully covered) but it is a
  hard ceiling.
- Antenna imbalance: `CHRMS ch0=60.0 ch1=28.8 ch2=151.9 ch3=171.6` — **branch 0 is the weakest**,
  ~9 dB below ch3. Relevant to any combining decision.

## 5. RETRACTED — do not cite

- ~~"The four-RX MRC skip blocks dedicated PDCCH decoding" (817 vs 8, 3202 vs 11).~~ Those counts
  were **`0x5199`, a degenerate polar-decoder fixed point on empty CCEs**, not a C-RNTI: identical
  payload every time (`rv=3 mcs=17 harq=8 startrb=62 numrb=129`) and it vanishes entirely under the
  energy gate. The A/B measured artifact production.
- ~~"The PDCCH receive chain is broken."~~ False — see §4.
- ~~2026-08-04's "frequency coherence is destroyed, fix FFT-window/time-tracking first".~~ That
  analysis shared the blind path's extraction assumptions; SIB1 decoding disproves it.
- ~~"The elevated energy in slots 3/4/5 is CSI-RS."~~ Wrong: CSI-RS is on symbols 4/8 and we extract
  symbol 0; slot-1 elevation is SIB1 bleeding into the overlapping 270-RB dedicated CORESET.
- ~~"`dci_length` is stale."~~ It is 47, confirmed against the live log.

## 6. Recipes

**Air check (run before AND after every capture):**
```bash
/usr/local/lib/uhd/examples/rx_samples_to_file --args "addr=192.168.20.2" --freq 3414990000 \
  --rate 30720000 --gain 40 --duration 0.3 --channel 0 --file /tmp/a.dat --type short
# then per-ms power: live cell ~20 dB spread, peak >1000; dead cell ~0.2 dB spread, peak ~60
```

**Self-detect the live C-RNTI (stage 1), then use it (stage 2)** — beats any quoted value:
run passive with `--sensing.pdcch_blind_monitor_rnti_range "1:65519"`,
`--sensing.pdcch_blind_monitor_noise_gates "0:1:0:0:0"`, `ISAC_PDCCH_FULLCRC=1`, and histogram
`crc=0x....`. **Then apply R1: confirm the payload varies and it survives the energy gate**, or you
have found another fixed point.

**Standard passive invocation:**
```bash
sudo -E env <PROBES> timeout 110 ./ran_build/build/nr-uesoftmodem \
  --usrp-args "type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.3,use_dpdk=1234" \
  -O /home/sens/NICOLA/nrue.ota.sensing.conf \
  -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 --ue-rxgain 40 \
  --ue-nb-ant-rx 4 --ue-nb-ant-tx 4 --passive-rx --ue-fo-compensation
```
(`sudo -E env VAR=...` — plain `sudo -E` does NOT pass the variables.)

**Probes added this session** (all default-off, read-only):
| env | what it prints |
|---|---|
| `ISAC_PDCCH_FULLCRC=1` | untruncated 24-bit CRC per candidate (`upper=` is the real/fake test) |
| `ISAC_PDCCH_CFGTRACE=1` (+`_SLOT=n`) | every derived extraction index per invocation |
| `ISAC_PDCCH_LLRPROBE=1` (+`_SLOT=n`) | LLR signature (`sum`, `nz`, first 8) + recovered CRC |
| `ISAC_PDCCH_ENERGY=1` | strongest candidate vs adaptive noise floor, per occasion |
| `ISAC_PDCCH_MRC4=1` | force 4-branch MRC on in either mode |
| `ISAC_PDCCH_CAPTURE=1` | write the offline replay fixture (pre-existing) |

## 7. Code changes kept

1. **`CoreSetType` fix** — was hardcoded `PDCCH_CONFIG`; that field's only RX effect is
   `dmrs_ref = BWPStart` vs `0`, so a CORESET0-style config with non-zero `bwp_start` generated
   DM-RS offset by that many RBs. New optional 7th `pdcch_blind_monitor_coreset` field
   (`...:1` = MIB_SIB1); default 0 keeps every existing config bit-identical.
2. **AL4/AL8 candidate sweep** — SIB1 sits at AL4 and was previously unreachable. Budget-guarded.
3. **Passive-mode MRC gate** — `nb_ant==4` skip now applies only when NOT `IS_PASSIVE_RX_MODE()`.
   **Its justification is now only first-principles** (branch 0 is the weakest antenna), because the
   measurement supporting it was retracted. Re-measure it properly or consider reverting.
4. Ported from `x410-100MHz`: PDCCH/PDSCH four-RX handling, 4-port rank-1 PMI, MAC layer-count
   fallbacks, actor core affinity. See `four-rx-mrc-overflow-is-the-x410-decode-bug` memory.

**A failed change, deliberately documented in-code so nobody rebuilds it:** replacing the MRC
cascade with an int32 equal-gain accumulator (`/nb_ant`) is a **REGRESSION** — the combiner feeds
`nr_pdcch_llr()` which **CLIPS at ±31**, so absolute amplitude sets soft-bit resolution. Dividing by
`nb_ant` drops the sum below the rail and LLRs collapse to 0/±1. Any re-attempt must renormalise to
the clip rail and re-measure.

## 8. The open problem, and the next step

**Why do dedicated grants never decode, when the extraction indices match ground truth exactly, the
candidate ladder covers the real AL1/CCE range, and energy is present in the band?**

The sharpest clue is §4's slot correlation: the gNB's busiest dedicated slots show no energy at our
receiver. That is unexplained and is where to start.

**Do NOT continue live-probing.** Three retractions came from inferring from aggregate statistics on
a stream where the RNTI, the cell state and the machine all kept changing. Use
**capture-and-replay**, which is already built and is the method that produced this project's last
real root cause:

1. `ISAC_PDCCH_CAPTURE=1` to freeze the fixture.
2. A gNB debug log covering **the same window** (`log: all_level: debug`; note the routinely-synced
   `gnb_remote_logs/gnb.log` is often WARNING level and the 4x4 yaml writes `/tmp/gnb-4x4-r1.log`).
   At debug this cell produces ~500 MB/min, so 30-60 s is plenty — or filter at source:
   `grep -a -m20 "with context NON" /tmp/gnb-4x4-r1.log`.
3. Score offline against known-true `(SFN, slot, CCE, AL, RNTI)`: for a grant the gNB says exists,
   did our extraction see energy at that CCE, and did it decode? That replaces every inference in
   this document with a direct per-instance answer.

Related memories: `sib1-oracle-proves-pdcch-rx-chain-broken`,
`four-rx-mrc-skip-blocks-passive-pdcch` (read its retraction section),
`sens6-is-the-x410-host-now`, `sens6-has-no-avx512-rebuild-copied-trees`,
`blind-pdcch-needs-al1-scanning`, `phase3-blind-pdcch-live-wiring-blocked` (superseded).
