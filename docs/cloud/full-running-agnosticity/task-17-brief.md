### Task 17: Lab-cell OTA validation with the X410

Validates Tasks 1–16 on air and measures slot coverage. Arms A/B run first on the cell as configured today (AL2); the user then reconfigures the lab gNB to AL1 (Step 6), and every other feature is exercised only where the lab gNB can be configured to emit it (Step 6b).

**Files:**
- Create: `/home/sens/NICOLA/captures/agnostic_metrics.sh` (captures/ is not under git)
- Update: memory files (Step 7)

**Interfaces:**
- Consumes: log lines from Tasks 1–7: `SIB1 decoded`, `multi-CORESET bank add`, `CORESET VERIFIED`, `Technique D CONVERGED`, `Technique D Qm oracle`, `CSIRS_BLIND CONFIRMED after N slots`, `AL1_VERIFY`, `autodiscover AL1 cover lap done`, `SWEEP: evicted CONVERGED`, `SWEEP: new per-RNTI context`, `blind PDCCH monitor summary: occasions=`.
- Produces: a results table (in the memory file and the final report).

- [ ] **Step 1: Write the metrics script**

```bash
cat > /tmp/agnostic_metrics.sh <<'EOF'
#!/bin/bash
# usage: agnostic_metrics.sh DUR_SECONDS RUN_DIR...   -- one line of counts per run
dur=$1; shift
for d in "$@"; do
  L="$d/run.log"
  occ=$(grep -o 'blind PDCCH monitor summary: occasions=[0-9]*' "$L" | tail -1 | grep -o '[0-9]*$')
  printf '%s sib1=%s bank=%s verified=%s techD=%s qm=%s csirs=%s csirs_slots=%s al1v=%s lapdone=%s evictC=%s newctx=%s occ_per_s=%s | %s\n' \
    "$(basename "$d")" \
    "$(grep -c 'lane ALs from SIB1 CSS\|(from SIB1)' "$L")" "$(grep -c 'multi-CORESET bank add' "$L")" \
    "$(grep -c 'CORESET VERIFIED' "$L")" "$(grep -c 'Technique D CONVERGED' "$L")" \
    "$(grep -c 'Technique D Qm oracle' "$L")" "$(grep -c 'CSIRS_BLIND CONFIRMED' "$L")" \
    "$(grep -o 'CSIRS_BLIND CONFIRMED after [0-9]*' "$L" | head -1 | grep -o '[0-9]*$')" \
    "$(grep -c 'AL1_VERIFY' "$L")" "$(grep -c 'AL1 cover lap done' "$L")" \
    "$(grep -c 'evicted CONVERGED' "$L")" "$(grep -c 'new per-RNTI context' "$L")" \
    "$(( ${occ:-0} / dur ))" "$(cat "$d/verdict.txt" 2>/dev/null)"
done
EOF
scp /tmp/agnostic_metrics.sh sens6:/home/sens/NICOLA/captures/agnostic_metrics.sh && ssh sens6 "chmod +x /home/sens/NICOLA/captures/agnostic_metrics.sh"
```

- [ ] **Step 2: Preconditions (all must hold before any run)**

Run and check each:
```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 nr-uesoftmodem oai_usrpdevif 2>&1 | tail -1"   # build BEFORE any capture
ssh sens6 "pgrep -af 'make|ninja|cc1' | grep -v grep || echo no-build-running"
ssh sens6 "sudo pgrep -af nr-uesoftmodem | grep -v grep || echo no-receiver; timeout 15 uhd_find_devices 2>&1 | grep -i claimed"
ssh sens4 "ps aux | grep -E '[g]nb -c'"          # lab gNB running; take its log path from /proc/<pid>/fd, not a fixed path
tmux capture-pane -t iperf -p | tail -3            # iperf running on sensnuc3 with non-zero throughput
```
Expected: build done and nothing compiling; no receiver running and X410 `claimed: False`; gNB process present; iperf showing traffic. If any fails, fix it or stop — a capture without traffic cannot validate Technique D.

- [ ] **Step 3: Run arm A (current defaults), 5 runs**

```bash
ssh sens6 "cd /home/sens/NICOLA/captures && REPO=/home/sens/NICOLA/adaptive-rx-UL-DL MGMT=192.168.1.140 NIC=enp2s0f1np1 \
  ARM=agn8_A CONF=/home/sens/NICOLA/captures/agnostic_ota_noprior.conf DUR=300 TRIES=5 NANT=1 RXG=49 \
  CARRIER=3450000000 SCAN=0 SSB=150 XENV='ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1' \
  bash run_arm.sh > /tmp/agn8_A.log 2>&1"
```
Launch it with `run_in_background`, then arm a `Monitor` on `/tmp/agn8_A.log` on sens6 for lines matching `verdict=|DONE|PREFLIGHT ABORT`. `run_arm.sh` handles the kill → settle → probe sequence between tries.

- [ ] **Step 4: Run arm B (`ISAC_AL1_COVER=1`), 5 runs**

Same command with `ARM=agn8_B` and `XENV='ISAC_CSIRS_BLIND=1 ISAC_CSIRS_BLIND_IDSWEEP=1 ISAC_AL1_COVER=1'`, output `/tmp/agn8_B.log`. Start only after arm A's `DONE`, 60 s settle, and `uhd_find_devices` showing `claimed: False`.

- [ ] **Step 5: Score both arms**

Run: `ssh sens6 "cd /home/sens/NICOLA/captures && ./agnostic_metrics.sh 300 agn8_A_* && echo --- && ./agnostic_metrics.sh 300 agn8_B_*"`

Pass criteria (per arm, over the 5 runs; count only runs whose `verdict` is not `VOID_NO_SIB1`/RF-stall — report VOID runs separately):
- `sib1` ≥ 1 in every counted run.
- `bank` ≥ 1 in ≥ 4/5 (dedicated CORESET discovered). Arm B must not be worse than arm A.
- `csirs` = 1 in ≥ 4/5, with `csirs_slots` reported (Task 3 target: confirmation within a few thousand slots of the IDSWEEP solve).
- `techD` ≥ 1 in ≥ 4/5; record how many runs had `qm` ≥ 1 before convergence.
- `evictC` = 0 in every run.
- Arm B: `lapdone` = 1 in every run where discovery started.
- Report `occ_per_s` for both arms (slot coverage; ~1,500 DL slots/s is full coverage at 30 kHz).
Cross-check at least one run against the live gNB log (C-RNTI, DCI 1_1 length, MCS table from the gNB config) — validation only, never fed back into the receiver.

- [ ] **Step 6: AL1 on air (the user changes `gnb.yaml`)**

The user has agreed to reconfigure the lab gNB's UE-specific search space to AL1. Wait for the user to confirm the change and the UE re-attach; then confirm AL1 in the live gNB log (resolve it from the running process; the log prints log2(L), so AL1 reads as `0`). Repeat Step 4 (arm B, 5 runs) and require `al1v` ≥ 1 in ≥ 4/5, `bank` ≥ 1, and report the `distinct_al1_families` values and any `AL1_UNION` lines (Task 15). Also run arm A (5 runs) on the AL1 cell: it shows what the receiver does without the cover.

- [ ] **Step 6b: Feature matrix — exercise each new decoder path where the lab gNB allows it**

For each row, check whether the OCUDU config on sens4 exposes the setting (read its config reference / existing `gnb.yaml` keys; do not guess key names). If it does, give the user the exact change to make and run 5 runs of arm B after it; if it does not, record "not configurable on OCUDU — offline tests only" for that row. Never edit `gnb.yaml` yourself.

| Feature (task) | gNB setting to look for | Pass on air |
|---|---|---|
| RA type 0 / dynamicSwitch (8–10) | PDSCH/PUSCH resource allocation type, RBG size | `dl_ldpc_ok` > 0 on type-0 grants; resolved `fdra_mode` matches the config |
| Interleaved VRB (11) | VRB-to-PRB interleaving, bundle size | CRC > 0 with `vrb=1` grants; `VRB_IL … latched` with the configured L |
| PRG (12) | PRB bundling size | `PRG … latched` equals the configured size, CRC not below arm A |
| DM-RS / data scrambling ID (13) | `scramblingID0`, `dataScramblingIdentityPDSCH`, PUSCH identity | estimator decides the configured IDs; CRC recovers after the data-ID walk |
| Mapping type B / k0 ≥ 2 (14) | TDRA list entries | Technique D converges on the configured entry |
| AL16 (15) | UE-specific search-space AL16 candidates | AL16 accepts with the real C-RNTI |
| CSI-RS rows 6–18 (16) | an 8/16/32-port NZP CSI-RS resource | `CSIRS_BLIND CONFIRMED` on the configured row/bitmap |

Restore every changed setting afterwards (the user does it; confirm in the gNB log).

- [ ] **Step 7: Record results**

Update memory: add a new file `agnostic-lab-ota-2026-09-25b.md` (results table + pass/fail per criterion), and correct the stale records found during the audit — `per-rnti-contexts-deferred.md` (now built, `2390064797`), `ul-decode-inline-on-rt-thread.md` (UL queue exists), `passive-pdcch-slot-coverage-is-the-blocker.md` (UL scan gate now honoured) — each with a one-line "superseded" note, and index them in `MEMORY.md`.

---

## Out of scope — standard or hardware limits, each with the trigger that brings it in

| Item | Why excluded | Trigger |
|---|---|---|
| Rank 5–8 (two codewords) | 4 RX antennas cannot separate >4 layers | a receiver with ≥8 RX |
| FR2 / 120 kHz SCS, extended CP | no FR2 front end; extended CP exists only at 60 kHz | FR2 hardware, or a 60 kHz ECP cell |
| DM-RS type 2 in the ID estimator (Task 13) | estimator is type-1/port-0 only; type-2 cells keep the PCI assumption | a cell whose layouts resolve `dmrs_type = 1` |
| LTE-CRS rate matching (DSS) | n78 cells do not run DSS | a DSS cell (low band) |
| SPS PDSCH / configured-grant PUSCH occasions without a DCI | only the CS-RNTI activation DCI is visible; later occasions carry no DCI | CS-RNTI activations seen in the blind monitor |
| Slot aggregation / PDSCH repetition combining | the rv0 transmission still decodes; only combining gain is lost | CRC below the gNB's delivered rate with aggregation configured |
| NR-U, multi-TRP, Rel-17+ features | not deployed on target cells | a target cell using them |
| GPU warp-per-candidate joint solver | Task 7 first; CPU wins below ~300 candidates/occasion | >300 joint solves per occasion after Task 7 |
