# Capture-path optimization plan (UL+DL passive receiver)

Scope: the **capture** path only — blind PDCCH scan, PDSCH/PUSCH decode, CFR extraction. The
sensing detector and fusion are out of scope by instruction and are not touched.

Everything below is measured on one 95 s OTA capture with UL iperf running,
`captures/prof_080521/prof.log`, branch `total-passive-rx-UL-DL` @ `acae048625`, 273 PRB / 4 RX /
X410, probes `ISAC_PDCCH_TIMING=1 ISAC_PUSCH_TIMING=1`. R7 applies: this run is for
characterisation, scoring runs must have the probes off.

---

## 1. What was measured

| path | thread | per-unit cost | volume | total CPU | over 500 µs slot |
|---|---|---|---|---|---|
| DL blind PDCCH scan (`BTIM`) | **PHY receive (RT)** | 69.4 µs / occasion | 106 000 | 7.36 s | 10 / 106 000 = 0.009 % |
| DL PDSCH decode (`PDSCHQ`) | consumer queue | — | 20 009 decoded, 0 dropped | — | max_lag 3/20 slots |
| UL PUSCH (`UTIM`) | **PHY receive (RT), in-line** | **1065 µs / grant** | 16 907 | **18.00 s** | **16 830 / 16 907 = 99.5 %** |

UL stage split, mean per grant:

```
fep 314 µs   rx_pusch 319 µs   cfr 29 µs   decode 403 µs   TOTAL 1065 µs (max 1459)
```

DL stage split, mean per occasion: `fep_llr 55.8 (80 %)  demap 2.0  prepass 1.5  decode 5.7`.

**RT-thread duty.** 7.36 s (DL) + 18.00 s (UL) = 25.4 s of passive-tap work in a ~66–85 s stream,
i.e. **~30–38 % duty**, on top of base OAI receive processing. `nr_pdcch_blind_monitor_rt.c:474-481`
already records this deployment holding PBCH lock below ~18 % duty and losing it above. We are
roughly 2× over that line, and this run took **4 RF stalls**, each needing a full device re-init,
each logging `pbch_ok=0 pbch_fail=50`.

That correlation is a hypothesis, not a conclusion — `rf_pow` collapsing 159 → 4.94 is also the
known X410 MPM-claim stall signature, which is not a CPU effect. Item 1 below is the experiment
that separates them.

## 2. The framing that matters: CPI wall time is grant-limited today, not CPU-limited

- 128 rows/CPI; `T_slot` median 3.449 slots/row → **0.22 s per CPI**.
- 36 917 grants (20 010 DL + 16 907 UL) over ~170 000 slots = **0.217 grants/slot**, and rows track
  grants roughly 1:1.
- Nothing is being dropped: `PDSCHQ dropped[full=0 stale=0]`, `max_lag_slots=3/20`, UL `unsup=2`.
- TDD ceiling is 9 usable slots in 10 → `T_slot` 1.11 → 0.071 s/CPI. That **3.1× headroom is
  realised by more grants, not by less CPU.**

So CPU optimization does **not** shorten a CPI at today's offered load. What it buys is:

1. **headroom** — the row rate can rise with traffic without the receiver falling over;
2. **stability** — removing a 99.5 %-over-budget condition on the receive thread;
3. **captured fraction** — 206 CPIs × 0.22 s ≈ 45 s of CPI coverage inside a 95 s process. The
   single largest loss of CPIs per unit time is the 4 stalls, not the per-CPI cost.

State this alongside any before/after number, or the optimization will look like it did nothing.

---

## 3. Ranked changes

### Tier 1 — structural, measured

**1. Move the UL PUSCH decode off the RT thread onto the existing deferred queue.**

- Removes 1065 µs × ~178 grants/s = **18.0 s of RT work**, and the 99.5 % over-slot condition
  outright. Largest single item by a wide margin.
- Reuse, do not rewrite: `nr_pdsch_passive_queue.{h,c}` already does exactly this for DL and is
  proven on this rig (20 009 jobs, zero drops, max_lag 3/20). Generalise it with a direction tag
  rather than adding a second queue — this is what the branch plan already specified.
- Constraint to carry over: the job holds a reference into `rxdata`, so staleness handling
  (`drop_stale`, `max_lag_slots`) applies identically. The DL queue refuses to start under
  `--cont-fo-comp`; we run `--ue-fo-compensation`, which is a different flag and is already proven
  compatible.
- **This is also the experiment that separates CPU-induced PBCH loss from the X410 stall**: if the
  4 stalls persist with RT duty down at ~10 %, they are the radio.

**2. Stop allocating ~4 MB of scratch per DL decode.**

Measured on the exact shapes (`/tmp/allocbench.c`, run twice, sens6):

```
alloc+clear+free per grant : 1469 µs   (3.96 MB: rxdataF_comp, dl_ch_mag/magb/magr, chest, llr)
same buffers reused, clear :   75 µs
```

**~1394 µs/grant, a 19.6× reduction.** The cost is `memalign` above glibc's mmap threshold: fresh
pages, faulted in one by one by the `memset` inside `malloc16_clear` (`common/utils/utils.h:77`),
then unmapped.

`allocCast2D/3D` is *designed* to be reused — `CheckArrAllocated` allocates only `if (!(ArraY))`
and `resizeAllowed` covers a shape change. `nr_pdsch_passive_decode.c:768-1030` defeats it by
declaring `fourDimArray_t *toFree = NULL;` as a **local** each call and `free()`ing at
`:1253-1257`. Make those pointers `static __thread` with `resizeAllowed = true`.

- Buffers: `toFree` (chest), `toFree2..5` (rxdataF_comp, dl_ch_mag, dl_ch_magb, dl_ch_magr), `llr`.
- Must be **thread-local**, not static: the queue runs N consumers.
- The 75 µs figure already assumes the buffers are still zeroed each call. Do not also drop the
  clear without proving each buffer is fully written before it is read.
- Caveat: this is a microbenchmark of the shapes, not an in-situ measurement — OAI's heap is warm
  and busy, so the real saving may be smaller. Confirm with `BTIM`/`PDSCHQ` before quoting it.

Same pattern, smaller: the UL FEP scratch (`nr_pusch_passive_decode.c:358-373`) is a 0.88 MB
`malloc16_clear` + `free16` **per grant**. Make it thread-local and persistent.

### Tier 2 — stop doing work nothing consumes

**3. Add a CFR-only UL mode.** `parse_ul_pusch` currently accepts only 0/1
(`nr_pdcch_blind_monitor.c:346`). Add `2` = channel-estimate and submit CFR, no LDPC.

- Skips `decode` 403 µs and most of `rx_pusch` 319 µs → UL cost **1065 → ~350 µs**.
- This is the honest configuration *right now*: with UL CRC at 0 % (commit `acae048625`) we are
  spending ~722 µs/grant on a decode that never succeeds and whose output nothing reads. The UL
  DM-RS CFR does not depend on it and keeps working — 16 907 submits, 52.0 M REs, unaffected.
- Keep mode 1 so the CRC can be re-measured once the decode is fixed.

**4. Turn on the deferred PDCCH scan queue.** It exists and is off (`scanq queued=0`);
`pdcch_blind_monitor_scan_thread = "n_consumers[:depth[:core]]"`, default 0 = in-line.

- Moves 69.4 µs × ~1247 occasions/s = **7.36 s** off the RT thread.
- The code's own header already argues for it with the same numbers.
- Do this *after* item 1, and measure separately — two changes at once on this rig is how a week
  got burned before.

### Tier 3 — only after measuring

**5. UL FEP writes into the gNB grid directly.** Currently FEP → scratch → `memcpy` per symbol per
antenna, ~0.88 MB/grant. `samples_per_slot_wCP == symbols_per_slot × ofdm_symbol_size`, so the
per-antenna shapes match and only `slot_off` differs — a shifted pointer may remove the copy
entirely. Verify the layout claim in code before relying on it.

**6. `max_ldpc_iterations` (currently 8, both directions).** Leave alone. The 403 µs UL decode is
long *because it never converges*; a converging decoder exits early on its own. Revisit only if the
UL CRC is fixed and decode time is still the limiter.

---

## 4. Order of work, and what to measure after each step

One change per capture, ≥5 runs per arm — this rig swings 0 %↔82 % between adjacent runs and a
1–2 run A/B has already produced a confident wrong conclusion twice.

| # | change | primary metric | expected |
|---|---|---|---|
| 1 | UL decode → deferred queue | `UTIM TOTAL` on RT, `over_slot`, RFSTALL count | RT duty ~38 % → ~10 %; over_slot → ~0 |
| 2 | thread-local decode scratch | `PDSCHQ max_lag_slots`, decode latency | queue latency down; RT unchanged |
| 3 | CFR-only UL mode | `UTIM TOTAL` | 1065 → ~350 µs |
| 4 | deferred scan queue | `BTIM TOTAL` on RT | −69 µs/occasion off RT |

Guard metrics that must not regress on any step: `PDSCHQ crc_ok` (94.2 % now),
`ul_cfr[submits/re]` (16 907 / 52.0 M), `LDPCDIAG ok` (18 842), CPI count, `T_slot`.

## 5. Out of scope but blocking a real result

- **UL PUSCH CRC is 0 %** (`acae048625`). Optimizing a decode that never succeeds is worth doing
  only because it makes it cheap to run the decode while diagnosing it. Leading candidate, predicted
  in the branch plan before any of this was built: this cell sends **UCI on PUSCH**
  (`harq_bit_len=7 beta_offset_harq_ack=6`), OAI's ULSCH receiver has no UCI support, and UCI bits
  displace ULSCH REs — so rate matching is wrong on exactly those grants. Test by counting grants
  with and without UCI before implementing TS 38.212 6.3.2.4 `G_ack` reservation.
- **4 RF stalls per 95 s run**, each a full device re-init. Costs more CPI coverage than every item
  above combined. Item 1 is the cheapest way to find out whether it is us or the radio.
