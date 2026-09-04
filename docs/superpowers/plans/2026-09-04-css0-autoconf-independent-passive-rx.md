# Fully Independent Passive RX (CSS0 Autoconf) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the passive receiver recover SI-RNTI grants from CORESET#0 with **no** hand-written `pdcch_blind_monitor_coreset/_ss/_bwp` lines in its conf, so it works on an unknown cell.

**Architecture:** Three changes, in dependency order. (1) Prove — by measurement, not argument — that the adaptive energy gate is what currently blocks every candidate, using the `ISAC_PDCCH_ENERGY` instrument that already exists but is not reachable from the runner. (2) Make `nr_pdcch_blind_monitor_autoconf_css0()` switch that gate off, on the already-established rule that autoconf must turn dedicated-path settings OFF rather than leave them unread; and record the `BWPStart` derivation in the log so it is never a copied constant again. (3) Remove the `init()` early-return that makes the three hand-written config lines mandatory, so `pdcch_blind_monitor_autoconf = 1` alone enables the monitor — the change that makes the independence claim true rather than merely intended.

**Tech Stack:** C (OAI PHY/MAC), C++ gtest (`test_nr_pdcch_blind_monitor`), CMake + Unix Makefiles, bash capture harness (`tests/passive_rx/captures/run_arm.sh`), USRP X410 on sens6 against a live srsRAN gNB on sens4.

**Spec:** `PHASE1_CSS0_AUTOCONF_HANDOVER.md` (repo root, sens6).

**Two corrections to that spec, established by reading code and the last capture before this plan was written. Both change what the work is:**

1. **The spec's "one open defect is `BWPStart`" is not supported by the evidence it cites.** In `/home/sens/NICOLA/captures/cfgtrace_090349/run.log` the summary line reports `candidates=199` at occasions=3000, 4000 **and** 5000 while `held[energy]` rises by exactly 3 per occasion (8801 -> 11801 -> 14801). `ENERGY_FLOOR_WARMUP` is `200` (`nr_pdcch_blind_monitor_rt.c:155`). So the candidate count froze at the moment the gate was permitted to reject, and the gate has rejected **100 %** of candidates ever since. Nothing has reached the polar decoder, so `BWPStart` has never been under test. Mechanism: `energy_floor_update()` (`nr_pdcch_blind_monitor_rt.c:159-172`) is fed by every candidate it tests; on the dedicated CORESET (45 groups / 270 RB) most candidates are empty so it tracks real noise, but CORESET#0 is 8 CCEs with SIB1 every 20 ms, so every AL4/AL8 candidate overlaps signal, the floor converges to signal level, and `energy_adapt_factor = 3.0` sits above it. Task 1 measures this; Task 2 fixes it.
2. **The spec's Definition of Done #3 is currently unreachable, and this is a code defect, not a config choice.** `nr_pdcch_blind_monitor_init()` returns early — leaving `g_enabled = 0`, which gates the whole RT tap at `nr_pdcch_blind_monitor_rt.c:425` — unless `pdcch_blind_monitor_coreset`, `_ss` **and** `_bwp` are all present and non-empty (`nr_pdcch_blind_monitor.c:824-826`). Delete them today and the monitor never runs at all. Task 3 removes that coupling.

**One suggestion in the spec that can be dropped:** §3 proposes attaching normally to get a control arm. Not needed. `run_arm.sh` already launches with `--passive-rx`, and the normal UE DL path still decodes MIB/SIB1 in that mode (`sib1=1` on the cfgtrace run's verdict line). The control arm is already in every capture.

## Global Constraints

- **Host and repo:** all work happens on **sens6**, in `/home/sens/NICOLA/openairinterface5g-total-passive-ue`, on branch `total-passive-rx-UL-DL-graphics`. Nothing in this plan runs locally.
- **Never build while a capture is running.** `pgrep -x nr-uesoftmodem` must return nothing before any `make`. 8 compile jobs on this host starve the receiver and the capture silently becomes a dead-cell measurement; this has produced one published wrong conclusion already.
- **Verify the binary, not the tree.** After every build that must reach the air: `strings cmake_targets/ran_build/build/nr-uesoftmodem | grep -c '<a literal you just added>'` must be non-zero. A clean `git status` is not proof the deployed binary carries the change.
- **`NANT=1` for every capture in this plan.** Receive branches 1-3 on this rig are individually undecodable (a paired in-run test rescued 0 of 15 411 transport blocks); they only add variance here.
- **Score with `ISAC_PDCCH_FULLCRC`, never with `accepts`.** Everything downstream of the CRC parses the payload as DCI **1_1**; SIB1 is **1_0**, so rejection there is correct and `accepts=0` is uninformative. Reading `accepts` as failure has cost this project two full debugging rounds.
- **A FULLCRC line is only a real decode if all three hold:** `upper=0x0` (on its own only a 1-in-256 test), the decoded payload **varies** between instances (an invariant payload is a degenerate polar fixed point — 3744 byte-identical "decodes" were once reported as success), and the candidate survived the energy gate when one is enabled.
- **`run_arm.sh` passes env through a `sudo env` allowlist.** A variable not named there is silently dropped and your setting does nothing.
- **`run_arm.sh`'s `verdict=` line does not score this work.** It voids on `dl_ldpc_ok=0`, which is the expected state for a CORESET#0-only run with no dedicated PDSCH. Use `TRIES=1` so it does not retry, and read the FULLCRC count.
- **Stop the receiver with SIGTERM and wait.** SIGKILL leaves a stale MPM claim on the X410; the next claimant makes MPM kill itself and the device is down ~100 s, presenting as a stream stall that is not one.
- **Target to beat:** `438 x FULLCRC L=4 dci_len=39 crc=0xffff upper=0x0 in_range=1` over 120 s, which is what this path produced when it last worked (2026-08-19).
- **Cell under test:** ARFCN 630000 / 3450 MHz, PCI 2, 273 PRB / 100 MHz, SCS 30 kHz, CORESET#0 = 48 RB / 1 symbol / 8 CCEs, SIB1 at slot index 1, SS0 period 40 slots.
- **Rig state confirmed 2026-09-04 before this plan:** X410 data plane `192.168.20.2` reachable; NIC `enp129s0f0np0` holds both `192.168.10.45/24` and `192.168.20.1/24` at MTU 9000; gNB running on sens4 as `/home/sens/OCUDU/build/apps/gnb/gnb -c /home/sens/gnb.yaml`.

---

### Task 1: Prove the energy gate is the blocker (no source change)

Make the existing `ISAC_PDCCH_ENERGY` instrument reachable from the runner, then take one capture that measures candidate energy against the floor per aggregation level, and one control capture with the gate off. This task changes no C code. Its deliverable is a measurement that either confirms the diagnosis in the header or refutes it — and if it refutes it, Task 2 must not be executed as written.

**Files:**
- Modify: `tests/passive_rx/captures/run_arm.sh:177-178` (the `sudo env` allowlist)

**Interfaces:**
- Consumes: nothing.
- Produces: two capture directories under `/home/sens/NICOLA/captures/`, and the `ENERGY=`/`CAPTURE=` runner knobs used by Tasks 2, 3 and 4.

- [ ] **Step 1: Confirm no capture is running, and commit the allowlist line already in the working tree**

`git status` shows `run_arm.sh` modified — that diff is the `FULLCRC`/`CFGTRACE` allowlist entry the handover already describes as "wired". It is not committed. Commit it before adding to it, so the two changes stay separable.

```bash
ssh sens6
pgrep -x nr-uesoftmodem && echo "CAPTURE RUNNING - STOP" || echo "clear"
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue
git diff tests/passive_rx/captures/run_arm.sh
git add tests/passive_rx/captures/run_arm.sh
git commit -m "captures: pass FULLCRC/CFGTRACE through the sudo env allowlist"
```

- [ ] **Step 2: Add `ENERGY` and `CAPTURE` to the allowlist**

Insert a new continuation line immediately after the line ending in `ISAC_PDCCH_CFGTRACE_SLOT=$CFGTRACE} \` (currently line 177):

```bash
    ${ENERGY:+ISAC_PDCCH_ENERGY=1} ${CAPTURE:+ISAC_PDCCH_CAPTURE=1} \
```

- [ ] **Step 3: Verify the knob is actually plumbed before spending a 150 s run on it**

A dry check that the variable survives the allowlist. Expected: the string appears.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
grep -n 'ISAC_PDCCH_ENERGY\|ISAC_PDCCH_CAPTURE' run_arm.sh
```

- [ ] **Step 4: Commit**

```bash
git add tests/passive_rx/captures/run_arm.sh
git commit -m "captures: pass ENERGY/CAPTURE through the sudo env allowlist

ISAC_PDCCH_ENERGY is the per-AL candidate-energy-to-floor probe and
ISAC_PDCCH_CAPTURE writes the replay fixture; neither was reachable from
run_arm.sh, so both read as 'no effect' rather than 'not passed'."
```

- [ ] **Step 5: Capture arm A — gate ON, energy probe ON (the measurement)**

150 s. This is the diagnostic run: it leaves the gate exactly as it is today and asks what the gate is looking at.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=egate_on CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=150 TRIES=1 NANT=1 ENERGY=1 FULLCRC=1 ./run_arm.sh
```

- [ ] **Step 6: Read arm A**

```bash
L=$(ls -dt /home/sens/NICOLA/captures/egate_on_*/ | head -1)/run.log
# does the candidate count freeze while held[energy] keeps climbing?
grep -aoE 'monitor summary: occasions=[0-9]+ candidates=[0-9]+ .*held\[energy=[0-9]+' "$L"
# per-AL candidate energy as a MULTIPLE of the measured floor -- al4 is where SIB1 lives
grep -aoE 'ENERGYPROBE .{0,90}' "$L" | tail -40
grep -ac FULLCRC "$L"
```

Expected if the header's diagnosis is right: `candidates` frozen near 199 across the whole run while `held[energy]` grows by 3 per occasion, **and** `ENERGYPROBE` `al4=` sitting close to or below `3.00` (the configured `energy_adapt_factor`) rather than well above it — i.e. the floor has risen to meet the signal.

**Decision point.** If `al4` is consistently well above 3.0 and candidates are *not* frozen, the diagnosis is wrong: stop, do not execute Task 2 as written, and report the measurement.

- [ ] **Step 7: Capture arm B — gate OFF (the control)**

Edit `/home/sens/NICOLA/nrue.passive_rx.autoconf.conf` line 46, changing the 5th field (`energy_adapt_factor`) from `3.0` to `0`:

```
  pdcch_blind_monitor_noise_gates = "0:2:500:0:0";   // TASK 1 ARM B: adaptive energy gate OFF
```

Then:

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=egate_off CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=150 TRIES=1 NANT=1 ENERGY=1 FULLCRC=1 ./run_arm.sh
```

- [ ] **Step 8: Score arm B against the three-part FULLCRC test**

```bash
L=$(ls -dt /home/sens/NICOLA/captures/egate_off_*/ | head -1)/run.log
grep -ac FULLCRC "$L"                                  # count
grep -aoE 'FULLCRC .{0,110}' "$L" | head -20           # part 1: upper=0x0, crc=0xffff, in_range=1
grep -aoE 'FULLCRC .{0,110}' "$L" | sort -u | wc -l    # part 2: payload MUST vary; 1 == artefact
grep -aoE 'monitor summary: occasions=[0-9]+ candidates=[0-9]+' "$L" | tail -3
```

Pass: a FULLCRC count in the same order as the historical 438 / 120 s, `crc=0xffff` (SI-RNTI) with `upper=0x0`, and more than one distinct line. Part 3 of the test is satisfied by construction here (no gate to survive), and is what Task 2 restores.

- [ ] **Step 9: Restore the conf and record the finding**

Revert line 46 to `"0:2:500:0:3.0"` — Task 2 makes the change in code, where it belongs, and leaving the conf edited would confound Task 3's run.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue
sed -i 's|pdcch_blind_monitor_noise_gates = "0:2:500:0:0";.*|pdcch_blind_monitor_noise_gates = "0:2:500:0:3.0";  // energy 3x floor ON, persistence K=2/500ms ON, SNR gate OFF (was discarding 97%)|' /home/sens/NICOLA/nrue.passive_rx.autoconf.conf
grep -n 'noise_gates' /home/sens/NICOLA/nrue.passive_rx.autoconf.conf
```

Append a dated subsection to `PHASE1_CSS0_AUTOCONF_HANDOVER.md` §2 recording: the frozen-`candidates` evidence, the `ENERGYPROBE` `al4`-to-floor numbers from arm A, the arm B FULLCRC count, and the explicit statement that `BWPStart` was never under test until now. Commit the doc alone (the conf is not tracked in this repo).

```bash
git add PHASE1_CSS0_AUTOCONF_HANDOVER.md
git commit -m "Phase 1: the blocker is the adaptive energy gate, not BWPStart

Measured: candidates freeze at ~199 (= ENERGY_FLOOR_WARMUP) while
held[energy] grows 3/occasion, so the gate rejects 100% of candidates and
no candidate has reached the polar decoder. energy_floor_update() is fed
by every candidate it tests; CORESET#0 is 8 CCEs with SIB1 every 20 ms, so
every sample is signal and the floor converges to signal level."
```

---

### Task 2: Autoconf switches the dedicated-path energy gate off, and records the BWPStart derivation

Two changes to `nr_pdcch_blind_monitor_autoconf_css0()`, both instances of a rule the module already follows for `dci01_scan` and `rnti_min/max`: a dedicated-path setting must be turned OFF under autoconf, not merely left unread. Plus one log field so the `BWPStart` value carries its own derivation.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:115-200` (`nr_pdcch_blind_monitor_autoconf_css0`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h:345-355` (add the `ssb_offset_point_a` parameter)
- Modify: `openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c:522-556` (the call site — pass the value it already computes)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc`

**Interfaces:**
- Consumes: `run_arm.sh`'s `ENERGY=`/`FULLCRC=` knobs from Task 1.
- Produces: `nr_pdcch_blind_monitor_autoconf_css0(int num_rbs, int num_symbols, int cset_start_rb, int ssb_offset_point_a, int ss_period_slots, int ss_slot, int ss_duration, int ss_first_symbol, int mux_pattern, int pci)` returning `bool` — a **10-argument** signature; `ssb_offset_point_a` is inserted as the 4th parameter. Task 3 does not change it again.

- [ ] **Step 1: Write the failing test**

Append to `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc`. Add `#include "nr_pdcch_blind_monitor_rt.h"` to the `extern "C"` block immediately after the existing `#include "nr_pdcch_blind_monitor.h"` — this compiles and links cleanly against the current test target (verified before this plan was written), and gives the test `nr_pdcch_blind_monitor_get_cfg()`.

```cpp
// ---- Phase 1: CSS0 self-configuration ---------------------------------------------------------
// The claim under test is not "autoconf sets the CORESET#0 constants" (it plainly does) but
// "autoconf switches OFF the settings that describe the DEDICATED search space". Leaving one on
// does active harm rather than nothing: dci01_scan consumed every surviving candidate and rejected
// all of them, and the adaptive energy gate rejected 100% of candidates forever (measured
// 2026-09-04: candidates frozen at 199 == ENERGY_FLOOR_WARMUP while held[energy] grew 3/occasion).
TEST(Css0Autoconf, TurnsOffEverySettingThatDescribesTheDedicatedSearchSpace) {
  auto* c = const_cast<nr_pdcch_blind_monitor_cfg_t*>(nr_pdcch_blind_monitor_get_cfg());

  // What nrue.passive_rx.autoconf.conf actually sets today for the dedicated CORESET.
  c->energy_adapt_factor = 3.0f;
  c->energy_min          = 2.0f;
  c->dci01_scan          = 1;
  c->rnti_min            = 1;
  c->rnti_max            = 0xFFEF;

  // This cell: CORESET#0 = 48 RB / 1 symbol at CRB 0, SSB at CRB offset 12 from point A,
  // SS0 period 40 slots / offset 0 / duration 2 / first symbol 0, mux pattern 1, PCI 2.
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48, 1, 0, 12, 40, 0, 2, 0, 1, 2));

  // The adaptive energy floor is estimated from the very candidates it gates. On the dedicated
  // CORESET (45 groups) most candidates are empty so it tracks noise; CORESET#0 is 8 CCEs with
  // SIB1 every 20 ms, so every sample is signal and the floor rises to meet it.
  EXPECT_FLOAT_EQ(c->energy_adapt_factor, 0.0f);
  EXPECT_FLOAT_EQ(c->energy_min, 0.0f);

  // Already-established behaviour, asserted here so a future edit cannot silently drop it.
  EXPECT_EQ(c->dci01_scan, 0);
  EXPECT_EQ(c->rnti_min, 0xFFFF);
  EXPECT_EQ(c->rnti_max, 0xFFFF);
}

// BWPStart is the frequency origin for both the DM-RS sequence and the RIV. A wrong value leaves
// every log line looking healthy, so the value must carry its derivation rather than be copied: it
// is cset_start_rb, which nr_mac_common.c:4078 defines as ssb_offset_point_a - rb_offset, and which
// OAI's own working normal path uses for coreset_id == 0 (nr_ue_dci_configuration.c:225).
TEST(Css0Autoconf, BwpOriginIsTheCoresetZeroStartNotTheSsbOrigin) {
  auto* c = const_cast<nr_pdcch_blind_monitor_cfg_t*>(nr_pdcch_blind_monitor_get_cfg());

  // A cell where the two differ, so passing the wrong one is detectable: CORESET#0 starts at
  // CRB 1 while the SSB sits at CRB 26 (the 2026-08-19 cell: offsetToPointA 26, rb_offset 25).
  ASSERT_TRUE(nr_pdcch_blind_monitor_autoconf_css0(48, 1, 1, 26, 40, 0, 2, 0, 1, 2));
  EXPECT_EQ(c->bwp_start, 1);
  EXPECT_EQ(c->bwp_size, 48);
  EXPECT_EQ(c->coreset_freq_domain, 8);
  EXPECT_EQ(c->coreset_type, 1);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
ssh sens6
pgrep -x nr-uesoftmodem && echo "CAPTURE RUNNING - STOP" || echo "clear"
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make test_nr_pdcch_blind_monitor -j8 && ./test_nr_pdcch_blind_monitor --gtest_filter='Css0Autoconf.*'
```

Expected: compile error — `nr_pdcch_blind_monitor_autoconf_css0` takes 9 arguments, not 10.

- [ ] **Step 3: Add the parameter to the header**

In `nr_pdcch_blind_monitor.h`, replace the declaration with:

```c
bool nr_pdcch_blind_monitor_autoconf_css0(int num_rbs,
                                          int num_symbols,
                                          int cset_start_rb,
                                          int ssb_offset_point_a,
                                          int ss_period_slots,
                                          int ss_slot,
                                          int ss_duration,
                                          int ss_first_symbol,
                                          int mux_pattern,
                                          int pci);
```

- [ ] **Step 4: Implement in `nr_pdcch_blind_monitor.c`**

Change the definition's parameter list to match Step 3. Then, immediately after the existing `g_cfg.dci01_scan = 0;` block, add:

```c
  /* The ADAPTIVE ENERGY GATE describes the dedicated CORESET and must be switched OFF here, for the
   * same reason dci01_scan is: leaving a dedicated-path setting on does active harm, not nothing.
   * energy_floor_update() is fed by every candidate it tests, so it only measures a NOISE floor when
   * most candidates are empty. That holds on the dedicated CORESET (45 groups / 270 RB) and fails
   * completely on CORESET#0, which is 8 CCEs with SIB1 every 20 ms: every AL4/AL8 candidate overlaps
   * the grant, the floor converges to SIGNAL level, and the threshold then sits above everything.
   * MEASURED 2026-09-04: candidates froze at 199 (== ENERGY_FLOOR_WARMUP) while held[energy] grew by
   * exactly 3 per occasion for the rest of the run -- a 100 % rejection rate that looked like a
   * decode failure and hid every downstream question, BWPStart included.
   *
   * OFF rather than retuned: the false-accept budget this gate defends is small here anyway. It was
   * sized for 8 CCE candidates x ~2000 slots/s on the dedicated CORESET; SS0 gives 3 candidates x
   * ~50 occasions/s, ~800x fewer trials, and the SI-RNTI pin above is worth ~16 bits of rejection on
   * its own. If a deployment ever needs a floor here, estimate it from CCEs OUTSIDE the monitored
   * candidates rather than from the candidates themselves -- that is the defect, not the factor. */
  g_cfg.energy_adapt_factor = 0.0f;
  g_cfg.energy_min          = 0.0f;
```

Then extend the existing `LOG_A` so the origin carries its derivation. Replace the format string fragment `"bwp=[%d..%d) "` with `"bwp=[%d..%d) (cset_start_rb = ssb_offset_point_a %d - rb_offset %d) "` and add the two matching arguments after `g_cfg.bwp_start + g_cfg.bwp_size`:

```c
        g_cfg.bwp_start, g_cfg.bwp_start + g_cfg.bwp_size,
        ssb_offset_point_a, ssb_offset_point_a - cset_start_rb,
```

- [ ] **Step 5: Update the call site**

In `openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c`, `update_pdcch_config()` already computes `ssb_offset_point_a` as a local before `get_type0_PDCCH_CSS_config_parameters()`. Pass it as the new 4th argument:

```c
    nr_pdcch_blind_monitor_autoconf_css0((int)t0c->num_rbs,
                                         (int)t0c->num_symbols,
                                         (int)t0c->cset_start_rb,
                                         (int)ssb_offset_point_a,
                                         (int)t0c->search_space_frame_period,
                                         css0_offset,
                                         (int)t0c->search_space_duration,
                                         (int)t0c->first_symbol_index,
                                         (int)t0c->type0_pdcch_ss_mux_pattern,
                                         (int)mac->physCellId);
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make test_nr_pdcch_blind_monitor -j8 && ./test_nr_pdcch_blind_monitor
```

Expected: all tests PASS, including the pre-existing `DciSize`, `Dci10Size` and `Dci00Size` groups.

- [ ] **Step 7: Build the receiver and verify the binary carries the change**

```bash
pgrep -x nr-uesoftmodem && echo "CAPTURE RUNNING - STOP" || echo "clear"
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make nr-uesoftmodem -j8
strings nr-uesoftmodem | grep -c 'ssb_offset_point_a'
```

Expected: non-zero. Zero means the deployed binary predates the edit and any capture taken on it is void.

- [ ] **Step 8: Live capture — gate now off by derivation, not by conf**

The conf still says `energy_adapt_factor = 3.0`; autoconf must override it. That override is the thing being tested.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=egate_autoconf CONF=/home/sens/NICOLA/nrue.passive_rx.autoconf.conf \
  DUR=150 TRIES=1 NANT=1 ENERGY=1 FULLCRC=1 ./run_arm.sh
```

- [ ] **Step 9: Score it**

```bash
L=$(ls -dt /home/sens/NICOLA/captures/egate_autoconf_*/ | head -1)/run.log
grep -aoE 'CSS0 autoconf .{0,260}' "$L" | tail -1     # must show the ssb_offset_point_a derivation
grep -ac FULLCRC "$L"
grep -aoE 'FULLCRC .{0,110}' "$L" | sort -u | wc -l   # >1 : payload varies
grep -aoE 'monitor summary: occasions=[0-9]+ candidates=[0-9]+ .*held\[energy=[0-9]+' "$L" | tail -3
```

Pass: `held[energy=0]`, `candidates` rising with `occasions`, a FULLCRC count in the same order as arm B of Task 1, `crc=0xffff`, and more than one distinct FULLCRC line. The logged `cset_start_rb = ssb_offset_point_a N - rb_offset M` must be arithmetically consistent.

- [ ] **Step 10: Commit**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c \
        openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h \
        openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_blind_monitor_test.cc \
        openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c
git commit -m "Phase 1: autoconf switches off the adaptive energy gate; log the BWPStart derivation

The adaptive floor is estimated from the candidates it gates, so it only
measures noise when most candidates are empty. CORESET#0 is 8 CCEs with
SIB1 every 20 ms, so the floor converges to signal and rejects everything:
measured candidates frozen at 199 (== ENERGY_FLOOR_WARMUP) with
held[energy] +3/occasion for the rest of the run. Same rule as dci01_scan --
a dedicated-path setting must be turned OFF, not left unread.

BWPStart is not copied: it is cset_start_rb = ssb_offset_point_a -
rb_offset (nr_mac_common.c:4078), the same value OAI's own normal path uses
at coreset_id == 0 (nr_ue_dci_configuration.c:225). Both terms are now in
the log line, so the value carries its own derivation.

FULLCRC decodes: <N> over <DUR> s, crc=0xffff, <M> distinct payloads."
```

---

### Task 3: `pdcch_blind_monitor_autoconf = 1` alone enables the monitor

This is the change that makes "fully independent" true. Today `nr_pdcch_blind_monitor_init()` returns early unless the three hand-written config lines are present, so the spec's Definition of Done #3 cannot be satisfied — deleting them disables the monitor entirely rather than handing it to autoconf.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:824-840` (the required-config early return) and `:933-948` (the DCI-width reconciliation block)
- Modify: `/home/sens/NICOLA/nrue.passive_rx.autoconf.conf` (untracked; delete lines 38-40)

**Interfaces:**
- Consumes: the 10-argument `nr_pdcch_blind_monitor_autoconf_css0()` from Task 2, and the `ENERGY=`/`FULLCRC=` knobs from Task 1.
- Produces: nothing new for later tasks.

- [ ] **Step 1: Write the failing test — a conf with no CORESET lines at all**

The receiver-side assertion is a live one (the config module is not exercised by the offline test), so the failing test is a capture. Copy the conf and delete the three lines that describe this cell's dedicated search space:

```bash
ssh sens6
cp /home/sens/NICOLA/nrue.passive_rx.autoconf.conf /home/sens/NICOLA/nrue.passive_rx.selfconf.conf
sed -i '/pdcch_blind_monitor_coreset *=/d; /pdcch_blind_monitor_ss *=/d; /pdcch_blind_monitor_bwp *=/d' \
  /home/sens/NICOLA/nrue.passive_rx.selfconf.conf
# must print nothing at all -- these three, and no near-miss such as _ul_bwp, may remain
grep -nE 'pdcch_blind_monitor_(coreset|ss|bwp) *=' /home/sens/NICOLA/nrue.passive_rx.selfconf.conf
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=selfconf_fail CONF=/home/sens/NICOLA/nrue.passive_rx.selfconf.conf \
  DUR=60 TRIES=1 NANT=1 FULLCRC=1 ./run_arm.sh

L=$(ls -dt /home/sens/NICOLA/captures/selfconf_fail_*/ | head -1)/run.log
grep -ac 'blind PDCCH monitor configured' "$L"   # expect 0
grep -ac 'blind PDCCH monitor summary'    "$L"   # expect 0
grep -ac 'CSS0 autoconf from MIB'         "$L"   # expect >0 -- autoconf runs, but into a dead monitor
```

Expected: the monitor never configures and never scans, while the autoconf line still prints. That asymmetry is the defect — autoconf populates a config that `g_enabled = 0` makes unreachable.

- [ ] **Step 3: Make the three config lines optional under autoconf**

In `nr_pdcch_blind_monitor.c`, replace the early-return block (currently at `:824-826`) and the three `parse_*` guards that follow it with:

```c
  /* The three lines below describe a DEDICATED search space, which a passive receiver can never
   * read off the air (it arrives in RRCReconfiguration over a ciphered SRB). Requiring them made
   * "self-configure from the MIB" unreachable: delete them and init() returned here, leaving
   * g_enabled = 0, which gates the entire RT tap (nr_pdcch_blind_monitor_rt.c). Autoconf would then
   * populate a config nothing ever read -- and it logged its success line while doing so.
   * g_cfg.autoconf is already set: config_get() above filled it via .iptr. */
  const bool have_manual_coreset = (p_coreset != NULL && p_coreset[0] != '\0'
                                    && p_ss != NULL && p_ss[0] != '\0'
                                    && p_bwp != NULL && p_bwp[0] != '\0');
  if (!have_manual_coreset && !g_cfg.autoconf) {
    return; // monitor disabled -- either the three lines together, or autoconf
  }
  if (have_manual_coreset) {
    if (!parse_coreset(p_coreset)) {
      LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_coreset '%s'\n", p_coreset);
      return;
    }
    if (!parse_ss(p_ss)) {
      LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_ss '%s'\n", p_ss);
      return;
    }
    if (!parse_bwp(p_bwp)) {
      LOG_E(PHY, "SENSING: malformed pdcch_blind_monitor_bwp '%s'\n", p_bwp);
      return;
    }
  }
```

- [ ] **Step 4: Do not let the DCI-width reconciliation fire on a not-yet-derived config**

The reconciliation block at `:933-948` calls `nr_pdcch_blind_dci_size(g_cfg.bwp_size)` with `bwp_size == 0` when there is no manual `_bwp`, producing a loud and wrong "every field is being read from the WRONG bit offset" warning at startup — precisely the "looks broken while healthy" noise this module works to avoid. Wrap the block:

```c
  if (have_manual_coreset) {
    // ... the existing { const uint16_t used_len = ...; } block, unchanged ...
  } else {
    LOG_I(PHY, "SENSING: blind PDCCH config deferred to CSS0 autoconf; DCI widths reconcile once "
               "the MIB has been decoded\n");
  }
```

- [ ] **Step 5: Build, verify the binary, and run**

```bash
pgrep -x nr-uesoftmodem && echo "CAPTURE RUNNING - STOP" || echo "clear"
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make test_nr_pdcch_blind_monitor nr-uesoftmodem -j8
./test_nr_pdcch_blind_monitor                                   # all still pass
strings nr-uesoftmodem | grep -c 'deferred to CSS0 autoconf'    # non-zero

cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=selfconf CONF=/home/sens/NICOLA/nrue.passive_rx.selfconf.conf \
  DUR=150 TRIES=1 NANT=1 ENERGY=1 FULLCRC=1 ./run_arm.sh
```

- [ ] **Step 6: Score against the Definition of Done**

```bash
L=$(ls -dt /home/sens/NICOLA/captures/selfconf_*/ | head -1)/run.log
grep -ac 'blind PDCCH monitor summary' "$L"            # >0 : the monitor is alive with no manual lines
grep -aoE 'CSS0 autoconf .{0,260}' "$L" | tail -1
grep -ac FULLCRC "$L"
grep -aoE 'FULLCRC .{0,110}' "$L" | sort -u | wc -l    # >1
grep -aoE 'monitor summary: occasions=[0-9]+ candidates=[0-9]+ .*held\[energy=[0-9]+' "$L" | tail -3
```

Pass: a FULLCRC count comparable to Task 2 step 9, from a conf containing none of the three lines.

- [ ] **Step 7: Prove the default is untouched**

Definition of Done #4: `pdcch_blind_monitor_autoconf = 0` must behave exactly as before. Run the pre-existing dedicated-path conf, which sets it to 0 and carries all three manual lines.

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=default_regression CONF=/home/sens/NICOLA/nrue.passive_rx.conf \
  DUR=150 TRIES=1 NANT=1 FULLCRC=1 ./run_arm.sh

L=$(ls -dt /home/sens/NICOLA/captures/default_regression_*/ | head -1)/run.log
grep -aoE 'blind PDCCH monitor configured.{0,200}' "$L" | tail -1
grep -aoE 'monitor summary: .{0,200}' "$L" | tail -1
grep -ac 'deferred to CSS0 autoconf' "$L"    # expect 0 -- the new branch must not be taken
```

If `/home/sens/NICOLA/nrue.passive_rx.conf` does not exist, use whichever conf in `/home/sens/NICOLA/` sets `pdcch_blind_monitor_autoconf = 0` and carries the three manual lines; confirm with `grep -l 'pdcch_blind_monitor_coreset' /home/sens/NICOLA/*.conf`.

Pass: the configured/summary lines report the manual values (`coreset(num_groups=45 ...)`, `bwp=[0..273)`), a non-zero `held[energy]` (the gate is back on where it belongs), and zero occurrences of the new deferred-config line.

- [ ] **Step 8: Commit**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue
git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c
git commit -m "Phase 1: autoconf alone enables the blind monitor

init() returned early unless pdcch_blind_monitor_coreset/_ss/_bwp were all
present, leaving g_enabled = 0, which gates the whole RT tap. So deleting
the three hand-written lines -- the actual claim of this phase -- disabled
the monitor rather than handing it to autoconf, while the autoconf success
line still printed into a dead config.

Verified with a conf carrying none of the three lines: <N> FULLCRC
SI-RNTI decodes over <DUR> s, <M> distinct payloads. Default path
(autoconf = 0) re-run unchanged."
```

- [ ] **Step 9: Update the handover document**

Rewrite `PHASE1_CSS0_AUTOCONF_HANDOVER.md` §2 (currently "THE OPEN DEFECT — one index") and §9 (Definition of Done) to record: `BWPStart = 0` is correct for this cell and why (both terms now logged); the energy gate was the blocker and is off under autoconf; the `init()` coupling that made DoD #3 unreachable; and the measured FULLCRC rate from Task 3 step 6 against the 438 / 120 s target. Move every hypothesis this plan settled into §2b with its evidence.

```bash
git add PHASE1_CSS0_AUTOCONF_HANDOVER.md
git commit -m "Handover: record the Phase 1 resolution and retire the BWPStart framing"
```

---

### Task 4 (conditional — execute only if Tasks 1-3 leave the FULLCRC rate below target): offline capture and replay

**Skip this task entirely if Task 3 step 6 met the target.** It is here because the spec's §5 proposes it as if it were a switch, and it is not: `ISAC_PDCCH_CAPTURE` writes `/tmp/pdcch_fixture.bin` (`dci_nr.c:772-805`) and **nothing in the repo reads it** — `grep -rl pdcch_fixture` matches exactly one file, the writer. A replay consumer is a build, so it is worth doing only if live runs have proven unable to resolve the remaining question.

**Files:**
- Create: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_replay.cc` (fixture reader + candidate decode loop)
- Modify: `CMakeLists.txt` (new `nr_pdcch_replay` executable beside `test_nr_pdcch_blind_monitor` at `:2351-2355`)

**Interfaces:**
- Consumes: `CAPTURE=1` from Task 1; the fixture header format `int32_t hdr[14]` beginning with magic `0x48434450` ("PDCH"), then `version=1`, `frame`, `slot`, at `dci_nr.c:800-805`.
- Produces: a standalone binary that replays one fixture and prints one line per candidate, so a `BWPStart` question becomes a sweep with an unambiguous answer rather than a 150 s round trip.

- [ ] **Step 1: Capture a fixture**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/tests/passive_rx/captures
ARM=fixture CONF=/home/sens/NICOLA/nrue.passive_rx.selfconf.conf \
  DUR=120 TRIES=1 NANT=1 CAPTURE=1 FULLCRC=1 ./run_arm.sh
ls -la /tmp/pdcch_fixture.bin
```

Non-empty is the precondition for everything below. The capture trigger needs `n_rb/6 > 4`; CORESET#0 gives 8, so it is satisfied.

- [ ] **Step 2: Get gNB ground truth for the same window**

Resolve both paths from the running process — they have moved mid-session before, and a stale log reads as "no traffic". The gNB must be at `log: all_level: debug` or the grant lines do not exist; check the level rather than assuming intent.

```bash
ssh sens4 "pgrep -a gnb"                                  # read its -c argument
ssh sens4 "grep -nE 'all_level|SI-RNTI' /home/sens/gnb.yaml | head"
ssh sens4 "grep -cE 'SI-RNTI' /home/sens/NICOLA/gnbLogs/gnb.log"
```

- [ ] **Step 3: Read the writer and pin the format before writing the reader**

```bash
ssh sens6 "cd /home/sens/NICOLA/openairinterface5g-total-passive-ue && sed -n '760,830p' openair1/PHY/NR_UE_TRANSPORT/dci_nr.c"
```

Write down the exact 14 header fields and the payload layout. Do not infer them; the reader must be derived from the writer, in the same commit-message style the rest of this module uses.

- [ ] **Step 4: Write the reader as a test with a known answer**

Assert on the fixture's captured occasion count and that at least one candidate CRC-recovers `0xFFFF` at `L=4` — the same three-part FULLCRC test, offline. Then sweep `BWPStart` over `{0, 1, 12}` and print the decode count per value; the correct origin is the one that decodes, and it is now a one-line sweep.

- [ ] **Step 5: Register the target and run it**

```bash
cd /home/sens/NICOLA/openairinterface5g-total-passive-ue/cmake_targets/ran_build/build
make nr_pdcch_replay -j8 && ./nr_pdcch_replay /tmp/pdcch_fixture.bin
```

- [ ] **Step 6: Commit**

```bash
git add openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_replay.cc CMakeLists.txt
git commit -m "Add an offline replay consumer for the blind-PDCCH capture fixture

ISAC_PDCCH_CAPTURE has written /tmp/pdcch_fixture.bin since it was added and
nothing has ever read it. A 150 s live run cannot resolve small differences
on this rig (adjacent runs have swung 0-82 % with config held fixed), so
config questions belong on a fixture with a known answer."
```
