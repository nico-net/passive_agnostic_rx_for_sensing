### Task 1: Commit the pending RNTI-cache and CSI-RS write-back fixes

Three files are modified but uncommitted on sens6 from earlier today: the evidence-protecting eviction in `rnti_ctx()` (+ `RNTI_CTX_MAX` 16→64, + regression test), the IDSWEEP scrambling-ID write-back into the pinned CSI-RS candidate, and the `null_median` display fix.

**Files:**
- Modify (already edited): `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c`, `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c`, `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc`

**Interfaces:**
- Consumes: nothing.
- Produces: a clean tree for every later task.

- [ ] **Step 1: Confirm the diff is exactly the three intended changes**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git status --short && git diff --stat"`
Expected: exactly these three files modified, nothing else.

- [ ] **Step 2: Build and run the affected tests**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 nr-uesoftmodem test_nr_pdsch_config_sweep test_nr_csirs_blind_synth 2>&1 | tail -3 && ctest -R 'test_nr_pdsch_config_sweep|test_nr_csirs_blind_synth' --output-on-failure | tail -5"`
Expected: build succeeds; both tests `Passed` (including `PdschConfigSweepRntiCache.EvictionProtectsEvidenceFromNoiseChurn`).

- [ ] **Step 3: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdsch_config_sweep_test.cc && git commit -F -" <<'EOF'
Technique D: protect evidence-bearing RNTI contexts from noise churn; CSI-RS: apply the solved scramblingID

rnti_ctx() evicted by pure LRU, so a burst of one-off blind-PDCCH RNTIs evicted the one real RNTI
5x in a 200 s lab run, wiping its prior/observations. Slots with evidence are now evicted only when
every slot has evidence; RNTI_CTX_MAX 16 -> 64. Regression test fails on the old policy.

The IDSWEEP-solved scramblingID was print-only: the candidate kept the PCI-derived id, so the
confirmation path kept scoring the wrong sequence. It is now written back. The periodic status line
printed the fixed 4/3 bar as "null_median"; it now prints null_median().

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

