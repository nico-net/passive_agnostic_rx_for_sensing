# X410 4-channel overflow — root cause of all PBCH/PDCCH/SIB1 failure (2026-08-06)

**Read this before touching PBCH, PDCCH, SIB1, the CIR timing loop, or the DMRS anchor.**

Branch `nrsniffer-pdcch-pdsch-migration`, HEAD `4e1bb8a3d7`, 14 commits, **unpushed**, tree clean.
Machine: **sens3** (`/home/sens/NICOLA/openairinterface5g-total-passive-ue`). X410 at
`192.168.20.2` over `enp101s0f1np1`.

---

## 1. The one-paragraph answer

At 4 channels × 122.88 MS/s the host periodically fails to consume samples fast enough. UHD
overflows, and **the stream then resumes about 5 seconds later with a new timestamp**. OAI advances
its frame counters by the samples it *received*, so the frame-to-sample mapping silently becomes
wrong and every downstream stage — CIR timing, PBCH, PDCCH, SIB1 — operates on the wrong samples.
Nothing detected it; PBCH simply stopped decoding.

**Every PHY-level component was measured and is correct.** Do not re-investigate them (§4).

**The remaining blocker is sustained host-side reception of 4 × 122.88 MS/s without UHD overflow.**
Everything else is downstream.

---

## 2. The evidence

Logged at the radio boundary from UHD metadata (`usrp_lib.cpp`, `SENSING: RFTSDISC`), before the
value reaches any OAI diagnostic. Events come in **strict pairs**:

```
n=1  uhd_err=8 ERROR_CODE_OVERFLOW  requested 61440  returned 56408  delta      +56,408
n=2  uhd_err=0 ERROR_CODE_NONE      requested  5032  returned  5032  delta +643,246,315
n=3  uhd_err=8 ERROR_CODE_OVERFLOW  requested 61440  returned 17011  delta      +17,011
n=4  uhd_err=0 ERROR_CODE_NONE      requested 44429  returned 44429  delta +615,263,514
```

- overflow → truncated read → **next read is clean but ~5.2 s later**
- 30 events per 90 s ≈ one pair every 4–6 s
- matches the measured lock duration exactly (§3)

**The clean read after an overflow carries `ERROR_CODE_NONE`.** Any detector must test the
**timestamp**, not the metadata, or it misses half the event.

**Not the network.** NIC `rx_missed_errors` delta is **0** over a full capture. (A separate,
already-fixed NIC problem is described in §7 — do not confuse them.)

**Not the instrumentation.** Tested, because the DMRS anchor runs ~550 FFTs per occasion on the PHY
thread and this was a genuine suspicion:

| | RFTSDISC | PBCH errors |
|---|---|---|
| anchor off | 38 | 2212 |
| anchor on | **18** | **239** |

Disabling it produces *more* discontinuities. The overflows are pre-existing and independent.

---

## 3. What works when the stream is continuous

The two-stage timing design is validated. After a coarse DMRS re-anchor, for 26 consecutive
occasions (~520 ms):

```
coh_at_pred  0.9973 – 0.9993     (valid-match level)
delta        −24 … +82 samples   (well inside the CIR estimator's ±288 capture range)
cir_resid    −10 … −14, stable   (the fine tracker doing real work)
```

Then an overflow lands and lock is lost within one occasion (coh 0.9992 → 0.8554). It never
recovers, because the bounded ±1-symbol anchor search cannot span a multi-second hole.

So: **the design is sound; the stream is not.**

---

## 4. Measured correct — do NOT re-investigate

Each of these was tested directly, not reasoned about:

| Component | Evidence |
|---|---|
| PBCH decode chain | 49/49 offsets decode across ±432 samples (golden-buffer replay) |
| CIR timing estimator | reports **+5** at known-correct alignment, correct sign; only *blind* beyond ±288 |
| All four antennas | each decodes PBCH alone on the golden snapshot (ch0 included) |
| `i_ssb` / `n_hf` | sweep selects the acquisition tuple and only that one |
| `ofdm_offset_divisor` | identical `CP/divisor = 36` on acquisition and tracking; no mismatch |
| Extraction indices, descrambling, polar decode, CRC masking | all pass in the golden control |

### Retracted hypotheses — do not reopen without new evidence

1. **N/2 CIR-origin bug.** Arithmetic matched beautifully (2048 + residual) but the estimator
   reports +5 at known-correct alignment. Coincidence.
2. **CFO-derived sample-clock drift (SFO).** CFO predicted 5.65 samples/frame; measured
   steady-state drift is ~0. CFO is dominated by the *gNB's* carrier offset, which never touches
   the X410 sample clock.
3. **Channel mapping / "signal is only on ch2".** One unreplicated whole-slot RMS reading. SSB-
   specific `chest_rms` shows all four healthy. **No hardware rework is needed.**
4. **Diagnostics breaking real-time reception.** Tested with everything gated off; acquisition and
   tracking are unchanged.
5. **"Tracking's buffer contains no SSB anywhere" (the original 2026-08-05 conclusion).** Does not
   reproduce; the scan finds it at 12.8–14.2×.

---

## 5. Tools built this session (all opt-in, default off)

### Normalized PBCH-DMRS coherence — the key instrument
`chest_coherence()` in `nr_pbch.c`. Adjacent-RE coherence of the channel estimate:
`|Σ conj(H[k])H[k+1]| / Σ|H[k]||H[k+1]|`.

**Calibrated on this cell:** correct DMRS **0.999**, wrong `i_ssb` **0.81–0.89**, live floor
**0.84–0.91**.

**`chest_rms` cannot do this** — it reads ~240 for correct *and* incorrect sequences alike, because
`Y·conj(X)` has similar magnitude either way. Several hours were lost trusting it. Likewise
occupied-band power is useless on a loaded cell, where every offset has energy.

### Env flags

| Flag | Effect |
|---|---|
| `ISAC_PBCH_TRACK=1` | re-enable PBCH tracking (disables the cached-MIB latch) |
| `ISAC_DMRS_ANCHOR=1` | two-stage coarse anchor + fine CIR tracking |
| `ISAC_ANCHOR_ENTER` / `_STAY` | hysteresis thresholds (default 0.99 / 0.97) |
| `ISAC_DISC_NO_RESYNC=1` | **temporary compat** — don't invalidate sync on discontinuity |
| `ISAC_PBCH_REPLAY=1` | golden-buffer capture + ±432 sweep |
| `ISAC_PBCH_2X2=1` | 2×2 cross-replay + per-antenna masks + `i_ssb` sweep |
| `ISAC_DELTA_SCAN=1` | per-occasion DMRS displacement measurement |
| `ISAC_CIR_TEST=1` | CIR convention test at known alignment |
| `ISAC_PHY_DIAG=1` | heavy per-symbol diagnostics (FRAMESCAN etc.) — **off by default, they are expensive** |
| `ISAC_TSYNC_MEASURE=1` | CIR loop measures without applying |
| `ISAC_CFO_DRIFT_SEED=1` | CFO-seeded drift bootstrap (**hypothesis falsified**, kept as evidence) |
| `ISAC_PBCH_OFFSET_BIAS=<n>` | PBCH-local FFT bias (diagnostic only) |

### Log tags
`RFTSDISC` (radio-boundary discontinuity), `RXDISCONT` (OAI-side, now triggers invalidation),
`POSTREBASE` (per-occasion coherence/delta/CIR residual), `ANCHOR`/`REBASE`, `PBCHMASK`,
`PBCHSWEEP`, `CIRTEST`, `TIMEMUT` (timing-state mutations with call site), `CHRMS`.

---

## 6. Next session — profile the RF consumer

**Do not debug PBCH/PDCCH/SIB1.** Their failures are downstream of the stream holes.

The decisive classification:

```
long RF-thread scheduling gap before overflow  → CPU scheduling / blocking / contention
continuous RF thread but depleted UHD buffers  → throughput, copy, conversion, buffer capacity
```

Measure, in order:

1. Max scheduling gap of the UHD receive thread immediately before each overflow.
2. RF-thread CPU affinity, RT priority, NUMA placement **relative to the NIC**.
3. Time blocked waiting for free OAI buffers vs time inside `recv()`.
4. Sample copies, interleaving, SC16→FC32 conversion in the 4-channel path.
5. UHD receive-buffer sizing; whether one thread handles all 4 channels in one multi-channel
   `recv()` rather than serialized work.
6. CPU frequency throttling, major page faults, involuntary context switches around events.

**Do not infer load from PBCH error counts.** They *dropped* when the receiver was doing less
useful work — that trap already produced one wrong conclusion.

Data rate for reference: 4 × 122.88 MS/s = 491.52 MS/s ≈ **1.97 GB/s** (SC16), 3.93 GB/s (FC32).

### After the overflow is fixed
- Re-enable sync invalidation (already default ON) and confirm it stays quiet.
- Re-run the anchor; expect sustained lock rather than 26 occasions.
- Only then investigate SIB1 (currently NACKs — the receiver reaches SI-RNTI grants but the
  transport block fails).
- Consider escalating the anchor state machine: LOCKED → SUSPECT → WIDE_SEARCH → REACQUIRE. A
  multi-second hole should force reacquisition, not a widened search.

---

## 7. Traps that already cost time

1. **STALE PLUGIN.** `usrp_lib.cpp` compiles into the dlopen'd `liboai_usrpdevif.so`.
   `ninja nr-uesoftmodem` **does not refresh it** — radio-boundary changes appear ineffective while
   the source is correct. Use `ninja nr-uesoftmodem oai_usrpdevif`. Same for
   `sensing_channel.c` → `librfsimulator.so` → `ninja rfsimulator`. Now in CLAUDE.md.
2. **`pkill -f nr-uesoftmodem` over SSH kills your own session** — the pattern matches the SSH
   command line. Use `pkill -9 -x nr-uesoftmodem`. Cost two runs.
3. **`sudo -E` alone does not pass env vars** — use `sudo -E env VAR=1 ...`.
4. **`--ue-nb-ant-rx 1` cannot acquire on this setup** (both normal and passive). Unresolved and
   separate; 4 antennas works. Not the channel-mapping theory (§4.3) — cause unknown.
5. **NIC tuning is not reboot-persistent by itself.** Made persistent via
   `/etc/sysctl.d/99-usrp-x410.conf` + enabled `x410-nic-tune.service` (MTU 9000, ring 8192,
   backlog 250000). Verify with `systemctl status x410-nic-tune` after a reboot. This fixed an
   earlier, *different* problem: 53 M `rx_missed_errors` and 61.5 % sample loss at MTU 1500.
6. **Cumulative counters hide faults.** Always measure a per-run *delta* of
   `/sys/class/net/<if>/statistics/rx_missed_errors`.
7. **Bounded log caps hide rare events.** `RXDISCONT` was capped at 40 lines, all consumed at
   startup, so it never showed the discontinuities it was built to find. Prefer event-driven
   logging for rare faults.
8. **Do not build while capturing** — compile jobs starve the UE and produce dead-cell captures.

---

## 8. Standing test command

```bash
ssh sens3
sudo pkill -9 -x nr-uesoftmodem; sleep 4
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
sudo -E env ISAC_PBCH_TRACK=1 ISAC_DMRS_ANCHOR=1 timeout 90 ./nr-uesoftmodem \
  -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 \
  --ue-rxgain 39 --ue-fo-compensation --ue-nb-ant-rx 4 --passive-rx \
  --usrp-args "type=x4xx,addr=192.168.20.2" \
  --uecap_file /home/sens/NICOLA/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml \
  -O /home/sens/NICOLA/nrue.ota.sensing.conf > /tmp/run.log 2>&1
echo "exit=$?"   # 124 = ran the full duration (expected)
grep -c RFTSDISC /tmp/run.log            # overflow/discontinuity events
grep -h POSTREBASE /tmp/run.log | tail -20   # per-occasion coherence and delta
```

Build: `cd .../build && ninja nr-uesoftmodem oai_usrpdevif`

---

## 9. Commit log (oldest → newest)

| Commit | Content |
|---|---|
| `0fc4f214ae` | diagnostics + retraction of "no SSB in buffer" |
| `733931190d` | golden-buffer replay — decode chain healthy, ±432 tolerance |
| `4f74b88d4a` | cached-MIB latch (**questionable**, see below) |
| `670c18a7b5` | CFO drift seed (**hypothesis falsified**, kept as evidence) |
| `9000fbb222` | CRC-independent timing gate + open-loop measurement |
| `4776f31c0f` | 2×2 cross-replay |
| `cdb0043b4d` | gate heavy diagnostics; per-channel RMS |
| `8009324419` | fix golden-snapshot frame bug; mandatory controls |
| `6aa5ba0c77` | `i_ssb`/`n_hf` sweep — no coherent DMRS at the used position |
| `5bc6c9bbac` | DMRS-coherence localisation — SSB found, +3620 away |
| `a34e7adbcc` | root cause: window converges ~2100 off |
| `f970aa454c` | CIR convention test — estimator correct, blind beyond ±CP |
| `47559867e9` / `31060f36c9` | two-stage anchor; symbol-3-vs-symbol-1 offset bug fixed |
| `97aec2abb8` | deferred atomic rebase — works, no assertion |
| `a3febed006` | post-rebase trace — Case 1, lock holds ~520 ms |
| `00f3097424` | timing-mutation + sample accounting — timestamp discontinuity found |
| `4bd24c6d1b` | raw UHD audit — overflow → restart pair identified |
| `4e1bb8a3d7` | sync invalidation (default ON, net-negative until overflow fixed) |

**`4f74b88d4a` should probably be reverted.** Its premise was that PBCH tracking is unfixable; that
is now known false. It also permanently prevents `nr_adjust_synch_ue()` from running, so every
timing experiment needs `ISAC_PBCH_TRACK=1`.

---

## 10. Open, in priority order

1. **Eliminate the UHD overflow** — blocks everything.
2. **`--ue-nb-ant-rx 1` cannot acquire** — unexplained, separate.
3. **SIB1 transport-block failure** — reaches SI-RNTI grants, TB fails. Blocked on (1).
4. **`+720 → +832` drift with the timing loop frozen** — small, unexplained, probably unrelated.
5. **Anchor state machine escalation** — WIDE_SEARCH / REACQUIRE tiers.
6. Nothing has been pushed. Decide whether to split the diagnostic commits from the functional ones
   before pushing.

---

## 11. Method note

Four confident hypotheses were falsified this session (§4), each after looking convincing —
including two exact arithmetic coincidences. In every case what killed it was **measuring the
specific quantity** rather than reasoning from the coincidence. Two conclusions were also retracted
after being reported, because a grep matched NACK lines and because a control failed in the same
run that produced the headline numbers.

Applying the same discipline to the overflow work is strongly advised: *"the host is too slow"* has
several distinguishable causes that look identical from the outside, and the profiling list in §6 is
ordered to separate them.
