# Blind PDCCH OTA decode — coherence handover (2026-08-04)

Branch: `total-passive-ue`, worktree `/home/sens/NICOLA/openairinterface5g`.
Frozen evidence + all analysis scripts: `/home/sens/NICOLA/pdcch_coherence_audit/`
(copied out of the session scratchpad — `/tmp` gets cleaned, this has been lost before).

---

## 1. The problem in one line

**No DCI decodes OTA.** The receiver *finds* the grant (right slot, right CCE, DM-RS matches) but the
data REs on that CCE equalise to noise, so the LLRs are garbage and CRC always fails. In rfsim the
same code decodes fine — this is OTA-only.

PDSCH is **blocked behind this**, not independently broken: passive data-aided PDSCH needs a valid
DCI, so it has never been reached OTA. Its 100 % rfsim CRC is an upper bound (rfsimulator hands every
client identical downlink samples), not a result.

---

## 2. Proven correct — DO NOT RE-SWEEP ANY OF THIS

Everything below was verified against the **transmitter's own source**, which is on this machine at
`/home/sens/NICOLA/repos/ocudu` — reading the gNB source beats inferring its behaviour, do that first
next time.

| thing | how it was confirmed |
|---|---|
| DM-RS generator | bit-exact vs an independent TS 38.211 implementation, \|corr\| = **1.000** |
| `c_init` formula | `dmrs_pdcch_processor_impl.cpp:14-20`, uses `slot_index()` = slot-within-frame |
| pilot indexing `m = rb*3+p` | `pdcch_processor_impl.cpp:98` gives `reference_point_k_rb = 0` for non-CORESET0 |
| `n_ID` = 2 | gNB log `nid_pdcch_dmrs=2` |
| CORESET geometry | `NON INTERLEAVED`, `bwp=[0..273)`, all-45 groups, `payload_size=47` |
| DM-RS RE positions | subcarriers 1/5/9 (`re_mask` in `dmrs_pdcch_processor_impl.cpp:39`) |
| subcarrier offset | swept ±24 absolute; **δ=0 wins clearly** (0.388 vs 0.19 control, next best 0.26) |
| CCE→RE indexing | 54/54 exact match, verified element-by-element |
| LLR slice/stride, unscrambling, polar call, CRC mask | audited, equivalent to OAI's working path |

### The decisive control (the one that settles it)

Using **only its own generated sequence**, the receiver picks the gNB-declared CCE as the top-scoring
CCE of its record:

| sequence | rank-0 rate |
|---|---|
| true slot, `n_ID`=2 | **16.8 %** |
| wrong slot +7 | 1.9 % |
| wrong slot +13 | 1.0 % |
| uniform null | 2.2 % |

Run it: `python3 allcce.py` (rank test) — the wrong-slot arms are in the session history and are
trivial to re-add. This is an experiment with a control, not a parameter-matching argument.

**Also established:** slot alignment is solid (8.8× vs 1.01× energy at the declared CCE). **Frame
alignment is NOT established** — every frame shift scores identically because the slot→CCE map is
quasi-static. Harmless: `c_init` has no frame dependence.

---

## 3. The actual defect

At CCEs the air confirms carry a grant:

- data REs carry the **same power** as the DM-RS (ratio 0.98, matching `power_control_offset_ss_db=0`)
  → the data is there;
- but the channel is barely coherent across frequency — ramp-aware leave-one-out pilot prediction
  error **0.700** (pure noise 1.08, a good channel 0.1–0.3), i.e. **~3 dB effective quality** while the
  CCE carries **18.9 dB** excess energy;
- and the data never forms QPSK under **any** channel estimate:

| channel estimate | blind QPSK statistic `\|mean((z/\|z\|)^4)\|` |
|---|---|
| OAI flat-per-RB (what ships) | 0.104 |
| delay-corrected | 0.106 |
| band-limited interpolation, all 810 pilots | 0.120 |
| *empty-CCE control* | *0.110–0.123* |
| *random baseline (54 REs)* | *0.136* |

**Strong power, destroyed frequency structure.** Since steps 2–8 of the decode chain are individually
verified correct, the corruption is upstream of the channel estimate — i.e. sync / FFT-window.

### Leading hypothesis (NOT yet confirmed)

Time tracking runs **only on a successful PBCH decode** —
`openair1/SCHED_NR_UE/phy_procedures_nr_ue.c:1146`:

```c
if (UE->no_timing_correction == 0 && pbchSuccess == 0) {
  sampleShift = nr_adjust_synch_ue(&UE->frame_parms, UE, pbch_ch_est_time, ...);
}
```

On a loaded cell PBCH decode fails (see memory `x410-pbch-tracking-bug-is-bandwidth-specific`,
traffic-gated, "tracking channel estimate is peaky vs acquisition"), so the FFT-window correction
silently **freezes and free-runs**. Secondary: `nr_adjust_synch_ue` only searches ±`nb_prefix_samples`
(=288 at N=4096) and `ue->max_pos_acc += max_pos` feeds `PID_I` with **no clamp** (unbounded
integrator).

This is elimination + a plausible mechanism, **not a measurement**. The instrumentation in §5 exists
to confirm or kill it.

---

## 4. RETRACTED — do not build on these

| claim | status |
|---|---|
| "DM-RS correlation at the declared CCE is at the noise floor" | **metric artifact.** The 3-RE-per-RB coherent metric has a 0.512 floor and assumes a flat channel across 8 subcarriers; a receive timing offset blinds it entirely |
| "per-RB correlation 0.954, DM-RS is recovered" | max over ~10M random CCE trials — a noise tail |
| all sweeps of slot / symbol / `n_ID` / indexing / conjugation / REG order | **VOID, not informative** — every one was scored with the blind metric above |
| "timing offset is the root cause" | offset is real but per-record; correcting it moves cv 0.713→0.609 and **QPSK not at all** |
| "delay exceeds the CP, so the ±288 tracking window is blind" | **tested and failed.** Records with \|τ\| inside the CP are no better (QPSK 0.099, LOO 0.965) than beyond it (0.119, 0.813) |
| "τ from the fixture measures the receive delay" | τ is a phase-slope fit to barely-coherent pilots — largely noise; the ±1024 "jumps" are wraps of the ±512 pilot ambiguity |

**Methodological rule that came out of this:** never accept a positive without a matched control and a
*measured* null distribution, and never trust a metric that has not been shown to return ~1 on a
known-good synthetic input.

---

## 5. Instrumentation added (built, verified present in the binary)

All **observation only** — nothing reads these values, no behavioural change. Off unless
`ISAC_TSYNC_AUDIT=1`. No CORESET / DCI length / `n_ID` / slot / symbol / pilot indexing / scrambling /
candidate ladder / CRC / polar code was touched.

| probe | file | fires | fields |
|---|---|---|---|
| `TSYNC_PBCH` | `SCHED_NR_UE/phy_procedures_nr_ue.c` | every SSB occasion | `pbch_ok`, `tracking_applied` |
| `TSYNC` | `PHY/NR_UE_ESTIMATION/nr_adjust_synch_ue.c` | every tracking update | `max_pos`, `global_pos`, `peak_out_of_window`, `e_win_frac`, `iir`, `acc` |
| `TSYNC_OUT` | same | every correction | `diff`, `sampleShift`, `sample_shift` |
| `TSYNC_PDCCH` | `PHY/NR_UE_TRANSPORT/dci_nr.c` | every PDCCH occasion | `coherence`, `tau_samples` |

`nr_adjust_synch_ue`'s own ±CP search is **unchanged**; a read-only full-symbol search was added
alongside it, so `peak_out_of_window=1` means the tracking loop is structurally blind to the true peak.

`coherence` is the load-bearing PDCCH field, not `tau`: τ is ambiguous beyond ±512 and meaningless
when coherence is low — which is exactly the failure regime.

---

## 6. Commands

### Build
```bash
cd /home/sens/NICOLA/openairinterface5g/cmake_targets/ran_build/build
cmake --build . --target nr-uesoftmodem -j6
# verify the probes actually linked in:
for s in TSYNC_PDCCH TSYNC_PBCH TSYNC_OUT peak_out_of_window; do
  strings nr-uesoftmodem | grep -q "$s" && echo "$s PRESENT" || echo "$s MISSING"; done
```
**Never build during a capture** — 8 compile jobs starve the active UEs of uplink, the gNB declares
them out-of-sync, and the capture silently becomes a dead-cell measurement. This has produced one
wrong conclusion already.

### Run the audit (needs a LOADED cell — the PBCH failure is traffic-gated)
```bash
cd /home/sens/NICOLA/openairinterface5g/cmake_targets/ran_build/build
sudo -E env ISAC_TSYNC_AUDIT=1 ./nr-uesoftmodem \
  -r 273 --numerology 1 --band 78 -C 3414990000 --ssb 165 \
  --ue-rxgain 39 --ue-fo-compensation --ue-nb-ant-rx 4 --passive-rx \
  --usrp-args 'type=x4xx,addr=192.168.20.2,mgmt_addr=128.178.122.154' \
  --uecap_file /home/sens/NICOLA/openairinterface5g/targets/PROJECTS/GENERIC-NR-5GC/CONF/uecap_ports1.xml \
  -O <sensing conf, see §7> > /tmp/ue_tsync.log 2>&1
```
`sudo` **scrubs the environment** — `-E` (or `sudo env VAR=...`) is mandatory or every probe stays
dark. This is the same trap that silently dropped `UE_NB_ANT_RX` during the AoA bring-up.

### Analyse
```bash
cd /home/sens/NICOLA/pdcch_coherence_audit
python3 tsync_audit.py /tmp/ue_tsync.log
```
Joins each PDCCH occasion to the most recent preceding PBCH occasion and tracking update, then splits
PDCCH coherence by `pbch_ok` and by `peak_out_of_window`. **The capture must contain PBCH failures**,
otherwise there is no contrast and the split is unanswerable — the script says so rather than
reporting a spurious result.

### Offline replay on the frozen fixture (no radio needed)
```bash
cd /home/sens/NICOLA/pdcch_coherence_audit
python3 allcce.py      # score every CCE from the air alone; rank of the declared CCE
python3 eqcheck.py     # constellation on air-confirmed grant CCEs
python3 fixproof.py    # OAI flat vs delay-corrected channel estimate
python3 timetrack.py   # per-record delay / coherence / QPSK, no averaging
```

---

## 7. Environment blockers (hit at the end of this session — fix these FIRST)

These are why the audit capture could not be taken. All post-date the `fix4.bin` capture (12:52) and
appear to be reboot-related.

1. **`/home/sens/NICOLA/nrue.ota.sensing.conf` is GONE.** This carried the whole blind-PDCCH + AoA
   setup. Known contents (from this session, values were live-verified at the time):
   ```
   pdcch_blind_monitor_coreset = "45:1:0:0:0:2";
   pdcch_blind_monitor_ss      = "1:0:1:0:22:0:0:0";
   pdcch_blind_monitor_bwp     = "0:273:0:47";
   pdcch_blind_monitor_tda     = "1:13,1:7";        // SIB1-verified: 2 entries
   pdcch_blind_monitor_dci_bits = "0:-1:-1:-1:-1:-1:-1:-1:-1:-1:-1:-1:-1";
   pdcch_blind_monitor_dmrs    = "2:1";             // gNB: dmrs_additional_position 2
   pdcch_blind_monitor_pdsch   = "2:1:0:1:2";
   pdcch_blind_monitor_noise_gates = "0:2:500:4.0:3.0";
   aoa_enable = 1;
   rx_array = "0,0;0.0351,0;0,0.0351;0.0351,0.0351";   // 2x2 UPA @ 0.4 lambda
   rx_array_boresight_deg = 0.0;                       // PLACEHOLDER, never surveyed
   ```
   Re-verify against the current gNB log before trusting — a wrong value here yields silent garbage,
   not an error.
2. **`/etc/uhd/uhd.conf` is GONE**, so DPDK is unconfigured. It held
   `dpdk_mac=b8:3f:d2:9d:a1:e6`, `dpdk_ipv4=192.168.20.45/24`. The ConnectX-5 is DPDK-bound, hence
   **absent from `ip link` and unpingable by design** — that is expected, not a fault. Without this
   file UHD falls back to the kernel stack, finds no route to 192.168.20.2, and dies with
   `RFNOC::MGMT EnvironmentError: IOError: Timed out getting recv buff for management transaction`.
   **This is the current hard blocker.**
3. **`net.core.rmem_max` / `wmem_max` reset to 62.5 MB on reboot.** Fixed this session with
   `sudo sysctl -w net.core.rmem_max=250000000 net.core.wmem_max=250000000`, but **not persisted** —
   add to `/etc/sysctl.d/` .
4. **gNB log mirror was stale** (`gnb_remote_logs/gnbLogs/gnb.log` ~2.3 h old). Re-`rsync` before
   reading the C-RNTI — it changes on every UE re-attach.
5. X410 recurring stall: ~50 % of starts hang; allow ~50 s between kill and restart.

---

## 8. Remaining work, in order

1. ~~Restore `/etc/uhd/uhd.conf` (DPDK) and the sensing conf~~ — **done, 2026-08-04**: `uhd.conf`'s
   `dpdk_mac` pointed at the physically unplugged NIC port (`e6`, no cable) instead of the connected
   one (`e7`); fixed. See §9.
2. ~~Take the audit capture... confirms or kills the PBCH-tracking hypothesis~~ — **done, 2026-08-04,
   hypothesis killed**: no separation between PDCCH coherence and PBCH tracking state; a direct causal
   test (force the full-symbol correction to actually apply) also failed to fix PBCH. See §9.
3. Only then revisit PDCCH decode. **Do not touch DCI/CORESET parameters** — §2.
4. **AL2 candidate ladder gap**: the ladder stops at CCE 36 (64-entry `CCE[]` array cap after 45 AL1
   entries) but the gNB actively uses CCE 42 (12,831 grants observed) — structurally unreachable today.
   Independent of the coherence bug and worth fixing regardless.
5. Set `rx_array_boresight_deg` from a real survey (currently a 0.0 placeholder — bearings are
   internally consistent but not world-frame referenced).
6. **Remove the instrumentation** when done: the `TSYNC*` probes, `ISAC_TSYNC_AUDIT`,
   `ISAC_FORCE_GLOBAL_SYNC`, `ISAC_PBCH_CAPTURE`, `ISAC_PBCH_CFO_SWEEP` gates (§9). Not yet — still
   the only working instrumentation for this defect.
7. Consider NTP-syncing sens3/sens4 — a 1188.9 s clock offset between them was a recurring source of
   cross-reference failures this session (it is why fixture↔grant alignment had to be done by SFN
   fingerprint rather than wall clock).
8. **The actual root cause is still unknown.** §9 closes five hypotheses (global timing offset, CFO,
   traffic load, cell-side health, OCUDU/OAI PBCH generation mismatch) with direct evidence, not just
   correlation. PBCH and PDCCH now look like the same symptom (strong power, ~0.22 coherence ceiling,
   decode never succeeds) with no known cause. Untested: FEP/FFT sample-indexing conventions after
   the recent decimation-path changes (though decimation itself is confirmed inert for this exact
   config, see §9.1), and PBCH RE-mapping/CRC/polar-code parameters (lower suspicion, spec-fixed,
   not yet diffed against OCUDU).

---

## 9. 2026-08-04 session: five hypotheses closed by direct test, root cause still open

Full session on `sens3` (the actual X410 hardware host — a *different* machine from wherever §1-§8
were written; see §9.0). Everything below is capture-only, opt-in instrumentation, gated on env
vars, zero behavioural change to production decode unless explicitly enabled. Landed in the
`total-passive-ue` branch (commit `OTA PDCCH/PBCH coherence investigation...`, both the sens3
worktree and this repo's own checkout — they had independently diverged, see §9.0).

### 9.0 Environment reality, not what §7 assumed

- The handover's `nrue.ota.sensing.conf` / `uhd.conf` guidance was written from a *different*
  machine's checkout than the one that actually holds the X410. `sens3` (100.118.99.39, not
  sensnuc3) is the real hardware host, and its `total-passive-ue` worktree
  (`~/NICOLA/openairinterface5g-total-passive-ue`) had **independently diverged** from this repo's
  checkout since a common ancestor (`71f3d7bc6c`) — different commits, different uncommitted work
  (a blind-PDCCH LLR-clipping fix, a CCE-mapping FPE fix, and a software RX-decimation feature, none
  of which exist here). The TSYNC instrumentation from §5 had to be ported over by hand.
- `uhd.conf`'s `dpdk_mac` pointed at `enp101s0f0np0` — confirmed via `ethtool` to have **no cable
  connected**. The X410 is actually wired to `enp101s0f1np1` (100Gb/s, up). A MAC-suffix typo (`e6`
  vs `e7`) between two ports on the same NIC. Fixed, backed up the original.
- **Software RX decimation is confirmed INERT for this deployment.** The warning that fires only
  when it activates (`"enabling Nx software RX decimation"`) never appears in any capture this
  session. `master_clock_rate=245.76MHz` is the device's internal ADC clock, not the granted stream
  rate — `get_rx_rate()` returns 122.88MHz natively via onboard DDC on this FPGA image
  (`fpga=UC_200`), so `decim_ratio` computes to 1 and the custom halfband-filter code added in an
  earlier session is dead code here. Nothing to test; not the cause of anything in this section.
- The X410's management IP silently changed mid-session (`128.178.122.154` → `.231`), implying an
  unlogged reboot/reconfiguration. Sync failures at a stale `--ssb` value that later resolved simply
  by re-deriving from a fresh gNB config dump were **not** evidence of a code bug — always re-verify
  `-C`/`--ssb` after any cell reconfiguration (ARFCN change, reboot), per the existing rule in the
  main CLAUDE.md.

### 9.1 The `−418` sample offset: retracted as a global receiver-timing effect

§3's leading hypothesis (PBCH decode gates FFT-window tracking, so a failure silently freezes
correction) does **not** survive direct testing:

- **Not reproducible via a different reference signal.** Added a `TSYNC_CSIRS` probe (identical
  coherence/tau estimator to `TSYNC_PDCCH`, applied to the raw CSI-RS LS estimate — i.e. *before*
  any of the sensing engine's own STO/CFO/SFO correction). Paired CSI-RS and PDCCH occasions within
  5ms of each other across 3 independent captures (~1000 pooled pairs): **raw correlation ≈ 0.03-0.04
  every time.** Two different reference signals measured near-simultaneously on the same receiver do
  not agree on any delay — inconsistent with a single shared receiver-wide timing state. (A naive
  "unwrap CSI-RS's ±170.7-sample ambiguity toward PDCCH's own reading" trick gives a spuriously high
  r≈0.95 — that is circular, snapping to whichever wrap happens to land nearest an unconstrained
  ±512-sample PDCCH reading, not a real finding. Flagged so it isn't mistaken for evidence later.)
- **Causal test, decisive.** `nr_adjust_synch_ue()`'s wide, unrestricted full-symbol peak search
  (`global_pos`, already computed for the `TSYNC` audit line) was normally read-only. Added
  `ISAC_FORCE_GLOBAL_SYNC=1`, which (a) feeds `global_pos` into the *actual* applied correction
  instead of the blind ±288-sample `max_pos`, and (b) — the fix that mattered — bypasses the
  call-site gate that only invokes the corrector `if (pbchSuccess == 0)` i.e. **only after a
  successful decode**, a structural deadlock when PBCH has never once succeeded. With the bypass,
  `TSYNC_OUT` fired on every occasion with real, converging corrections (177→92, 183→92, 144→72
  samples across successive occasions). **PBCH still failed 100% (0/5666).** A verified, non-inert
  intervention that changed nothing.
- **Verdict: −418 is a PBCH-tracker-specific artifact** (most likely of `nr_adjust_synch_ue()`'s own
  narrow-window peak search reading noise when the true peak sits outside it), not a receiver-wide
  timing offset. Do not build further fixes on it.

### 9.2 Residual CFO: real, stable, large — and definitively not causal

- Added `TSYNC_CFO_PBCH`: logs the PBCH CFO estimate (`nr_ue_pbch_freq_offset()`, already existed,
  previously only computed after success) unconditionally, every occasion.
- **Measured: −4590 to −4610 Hz, stable, for 134 of 144 captured seconds** after a ~10s acquisition
  transient. Std ~5% relative — a real, physical, persistent residual (15% of the 30kHz subcarrier
  spacing), not noise. `--ue-fo-compensation` (one-shot, acquisition-time only) is not holding it;
  `--cont-fo-comp` was never enabled this session.
- **First test (methodologically incomplete, superseded — kept for the record):** derotated
  `rxdataF`, i.e. *after* the FFT. A post-FFT rotation only removes CFO's common-phase term; it
  cannot correct inter-carrier interference (ICI), which is baked into the FFT output before any
  post-hoc rotation can touch it. Result (`crc_ok=0` at every hypothesis including the measured
  value) was correctly flagged as ambiguous between "CFO not causal" and "incomplete correction",
  not trusted as final.
- **Corrected test, decisive.** `ISAC_PBCH_CFO_SWEEP=1`: derotates the **raw time-domain samples**
  (`ue->common_vars.rxdata`) before FFT and re-invokes the real `nr_slot_fep()` /
  `nr_pbch_channel_estimation()` / `nr_generate_pbch_llr()` / `nr_pbch_decode()` verbatim on the
  corrected copy (shadow buffers throughout; `nr_pbch_decode` called with `ue=NULL`, an
  already-supported null-checked path, so nothing here can write back into UE state). Window-bounded
  using the real `get_samples_slot_timestamp()` call plus a generous margin — not a reimplementation
  of `nr_slot_fep`'s offset arithmetic — so there is no risk of silently reading the wrong samples.
  Swept **−8kHz to +8kHz in 500Hz steps (33 hypotheses × 10 occasions = 330 shadow decodes)**,
  reporting coherence (sym0-vs-sym2 channel-estimate correlation), mean LLR magnitude, and CRC per
  hypothesis:

  | metric | response across the entire ±8kHz sweep |
  |---|---|
  | coherence | flat at 0.22 ± 0.05-0.07 — **no peak anywhere**, including at −4600Hz |
  | LLR reliability | flat at 0.56 ± 0.02 — no variation at all |
  | CRC | 0/330 |

  Sign convention independently verified (not assumed) by tracing `dot_product()`'s actual
  conjugate convention through to `nr_ue_pbch_freq_offset()`'s formula before trusting the null
  result. **Flat metrics after a correct pre-FFT correction → CFO is not the cause**, per the
  decision rule this test was designed against. The 0.22 coherence floor is ~3× above the pure-noise
  level (~0.065 for 240-dim vectors) — a real partial-coherence signature exists, it is just
  completely insensitive to CFO.

### 9.3 Other hypotheses closed this session

- **Traffic load**: iperf off → still 0% PBCH success. Not traffic-gated, contra the earlier
  project assumption recorded elsewhere in this repo's memory.
- **Cell-side RF health**: commercial UE on the same cell shows CQI 11-12, RI=4 (stale gNB log,
  ~2.5h old at time of reading, but unambiguous) — the cell is healthy for a normal receiver.
- **Channel magnitude/SNR**: the pre-existing `PBCHFAIL` diagnostic (`nr_pbch.c`, `LOG_E`, capped at
  6/run) shows failing-occasion `|H|mean` (40.8-46.6) matches or slightly *exceeds* the
  successful-acquisition reference (44.5), zero saturation. Not a weak-signal problem.
- **RX stream discontinuity**: 45 `ERROR_CODE_OVERFLOW` events in a 150s capture — real, but far too
  infrequent (≈1 per 130 PBCH occasions) to explain a 100% failure rate.
- **OCUDU/OAI PBCH generation mismatch** — checked three components against OCUDU's actual TX
  source (`repos/ocudu`), same method already validated for PDCCH's DM-RS in §2:
  - **DM-RS scrambling `c_init`**: found and fixed a real bug — `nr_gold_pbch()`
    (`openair1/PHY/NR_REFSIG/refsig.c`) added the half-frame bit (`n_hf<<2`) to `i_ssb`
    unconditionally; per 3GPP TS 38.211 §7.4.1.4.1 and OCUDU's own
    `dmrs_pbch_processor_impl::c_init()`, it must only be added for `L_max==4`, never for
    `L_max==8/64`. **Confirmed inert for our failures**: the tracked SSB is always index 0 in the
    first half-frame, so `n_hf=0` either way — but a real spec deviation, kept fixed.
  - **Payload scrambling** (the `v`/`M`/unscrambling-mask self-referential mechanism in
    `nr_pbch_decode()`): verified bit-for-bit against OCUDU's `pbch_encoder_impl::scramble()` — the
    `nushift` bit-extraction positions (24, 6) and the XOR-vs-add combination are algebraically
    identical to OCUDU's `2*a[G[7]]+a[G[8]]`. No mismatch.
  - **Payload interleaving**: OAI's `pbch_deinterleaving_pattern[32]` verified as the exact inverse
    of OCUDU's `payload_generate()` G-array interleaver via symbolic simulation (tracked source
    labels through both algorithms, not hand-traced — an earlier hand-trace attempt made an
    indexing error). All 24 payload bits and 8 extra (SFN/HRF/SSB) bits land exactly where expected.
    No mismatch.

### 9.4 Where this leaves things

Five plausible causes eliminated with direct evidence (not just absence of correlation): global
timing offset, CFO, traffic load, cell-side health, OCUDU/OAI generation mismatch (DM-RS scramble,
payload scramble, payload interleaving). PBCH and PDCCH now read as the same underlying symptom —
strong received power, a real but low (~0.22-ceiling / near-noise-floor) coherence, decode never
succeeds — with no confirmed cause. Not yet checked: CRC24C computation, polar code
frozen-bit/rate-matching parameters, PBCH RE-to-subcarrier mapping (all lower-suspicion, spec-fixed
components, not yet diffed against OCUDU the way DM-RS/scrambling/interleaving were), and FEP/FFT
sample-indexing conventions specifically in light of the software-decimation code that landed in
this branch since §1-§8 were written (confirmed inert for the *current* config, §9.1, but worth
re-checking if the deployment ever changes rate/image).
