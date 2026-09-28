### Task 5: Walk the AL1 cover first in the lookahead lanes (opt-in `ISAC_AL1_COVER=1`)

Lanes walk (extent × mapping) serially — measured ~3 hypotheses/min, ~56 min for one pass of 271 mappings. With `ISAC_AL1_COVER=1` the lanes first do one lap over only the cover mappings, scanning AL1 only; then the existing staged walk (pass 0 → full) continues unchanged, so AL2+ discovery is not lost. On AL1 verification a diagnostic line reports how many AL1 families remain consistent (1 = the banked mapping decodes every AL1 candidate).

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h` (geom struct)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c` (lane struct, stage, `lane_map_count`, `lane_catalog_map_max`, `lane_assign_next`, `lookahead_lanes_init`, `nr_pdcch_blind_lookahead_get`)
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c` (force AL1 for cover lanes; verification diagnostic)
- Test: `openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc`

**Interfaces:**
- Consumes: `nr_pdcch_al1_cover`, `nr_pdcch_al1_enumerate`, `nr_pdcch_al1_regset`, `nr_pdcch_al1_narrow`, `nr_pdcch_al1_family_count`, `nr_pdcch_al1_map_t`, `NR_PDCCH_AL1_MAX_COVER`, `NR_PDCCH_AL1_MAX_MAPS` (Task 4).
- Produces: `bool al1_only;` in `nr_pdcch_lookahead_geom_t`; log lines `autodiscover AL1 cover lap done` and `AL1_VERIFY …` (grepped by Task 17).

- [ ] **Step 1: Write the failing test (lane-facing conversion)**

The cover is handed to the lanes as `nr_pdcch_map_cand_t {bundle, interleaver, shift}`; the conversion must preserve all three fields and the non-interleaved lead. Add to `tests/nr_pdcch_al1_map_test.cc`:

```cpp
TEST(Al1Map, CoverSurvivesTheLaneTypeRoundTrip) {
  // Mirrors al1_cover_for_span() in nr_pdcch_blind_monitor.c: same field order, same widths.
  struct lane_map { uint8_t bundle; uint8_t interleaver; uint16_t shift; };
  static_assert(sizeof(lane_map) == sizeof(nr_pdcch_al1_map_t), "layout drift");
  std::vector<nr_pdcch_al1_map_t> cov(NR_PDCCH_AL1_MAX_COVER);
  const int nc = nr_pdcch_al1_cover(216, 2, cov.data(), (int)cov.size());
  ASSERT_EQ(nc, 11);
  for (int i = 0; i < nc; i++) {
    const lane_map l = {cov[i].bundle, cov[i].interleaver, cov[i].shift};
    EXPECT_EQ(l.bundle, cov[i].bundle);
    EXPECT_EQ(l.interleaver, cov[i].interleaver);
    EXPECT_EQ(l.shift, cov[i].shift);
  }
}
```

- [ ] **Step 2: Run it**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make test_nr_pdcch_al1_map 2>&1 | tail -1 && ./test_nr_pdcch_al1_map --gtest_filter='*RoundTrip*' 2>&1 | grep -E 'PASSED|FAILED'"`
Expected: PASS (this pins the contract the wiring below relies on).

- [ ] **Step 3: Wire the cover lap into the monitor**

In `nr_pdcch_blind_monitor.h`, add to `nr_pdcch_lookahead_geom_t` after `bool fast_length_only;`:

```c
  bool al1_only;          // AL1 cover lap: scan aggregation level 1 only (ISAC_AL1_COVER=1)
```

In `nr_pdcch_blind_monitor.c`:

1. Add `#include "nr_pdcch_al1_map.h"` with the other local includes.
2. Add `bool al1_only;` to `nr_pdcch_lookahead_lane_t` (the struct closed by `} nr_pdcch_lookahead_lane_t;` ~line 884).
3. Next to the other `s_lane_dispatch_*` statics add `static int s_lane_after_cover_stage = 0;` and, above `lane_map_count()`:

```c
/* ISAC_AL1_COVER=1: before the staged mapping walk, one lap over only the AL1 COVER of each extent,
 * scanning AL1 only (nr_pdcch_al1_map.h: 2-11 mappings reproduce every AL1 candidate of an 81-1081
 * mapping catalogue). Default off: discovery order unchanged unless asked for. */
static int al1_cover_enabled(void)
{
  static int v = -1;
  if (v < 0) {
    const char *e = getenv("ISAC_AL1_COVER");
    v = (e != NULL && atoi(e) == 1) ? 1 : 0;
  }
  return v;
}

/* Covers depend only on (span, duration): cache the last few so each extent pays once. */
static int al1_cover_for_span(int span_rb, int duration, nr_pdcch_map_cand_t *out)
{
  static struct { int span, dur, n; nr_pdcch_al1_map_t c[NR_PDCCH_AL1_MAX_COVER]; } cache[8];
  static int next_slot = 0;
  int at = -1;
  for (int i = 0; i < 8; i++)
    if (cache[i].n > 0 && cache[i].span == span_rb && cache[i].dur == duration) { at = i; break; }
  if (at < 0) {
    at = next_slot;
    next_slot = (next_slot + 1) % 8;
    cache[at].span = span_rb;
    cache[at].dur = duration;
    cache[at].n = nr_pdcch_al1_cover(span_rb, duration, cache[at].c, NR_PDCCH_AL1_MAX_COVER);
  }
  for (int i = 0; i < cache[at].n; i++)
    out[i] = (nr_pdcch_map_cand_t){cache[at].c[i].bundle, cache[at].c[i].interleaver, cache[at].c[i].shift};
  return cache[at].n;
}
```

4. In `lane_map_count()`, directly after `const int span_rb = …;` add:

```c
  if (s_lane_dispatch_stage < 0)
    return al1_cover_for_span(span_rb, g_cfg.coreset_duration, out);
```

5. At the top of `lane_catalog_map_max()` add (an upper bound, so no cover is computed for every extent at init on the receive thread):

```c
  if (s_lane_dispatch_stage < 0)
    return NR_PDCCH_AL1_MAX_COVER;
```

6. In `lane_assign_next()`, as the first statement inside `if (s_lane_dispatch_map >= s_lane_dispatch_map_max) {`, add:

```c
      if (s_lane_dispatch_stage < 0) {
        s_lane_dispatch_stage = s_lane_after_cover_stage;
        s_lane_dispatch_ext = 0;
        s_lane_dispatch_map = 0;
        s_lane_dispatch_phase = 0;
        s_lane_dispatch_map_max = lane_catalog_map_max();
        LOG_A(PHY, "SENSING: autodiscover AL1 cover lap done, continuing with the staged mapping walk\n");
        continue;
      }
```

and after `ln->fast_length_only = (s_lane_dispatch_stage == 0 && map_staging_enabled());` add:

```c
    ln->al1_only = (s_lane_dispatch_stage < 0);
```

Extend the `ISAC_DISCOVER_DIAG` `LOOKAHEAD_ASSIGN` log with ` al1=%d` and argument `ln->al1_only`.

7. In `lookahead_lanes_init()`, replace

```c
  s_lane_dispatch_stage = (map_env != NULL && atoi(map_env) == 0) ? 1 : 0;
```

with

```c
  s_lane_after_cover_stage = (map_env != NULL && atoi(map_env) == 0) ? 1 : 0;
  s_lane_dispatch_stage = al1_cover_enabled() ? -1 : s_lane_after_cover_stage;
```

8. In `nr_pdcch_blind_lookahead_get()`, after `out->fast_length_only = ln->fast_length_only;` add `out->al1_only = ln->al1_only;`.

- [ ] **Step 4: Force AL1 and report on verification in the RT path**

In `nr_pdcch_blind_monitor_rt.c`:

1. Add `#include "nr_pdcch_al1_map.h"` next to `#include "nr_pdcch_joint_live.h"`.
2. In the lane loop, the geometry comes from `nr_pdcch_blind_lookahead_get(lane, &geom)` (~line 4536). Confirm that `geom` is still in scope at `uint8_t ln_al_active = …` (`grep -n 'ln_al_active' nr_pdcch_blind_monitor_rt.c`), then add directly after that line:

```c
    if (geom.al1_only)
      ln_al_active = 1; /* AL1 cover lap: the cover is only complete for aggregation level 1 */
```

3. Above the function containing the lane verification (`nr_pdcch_blind_lookahead_observe` call ~line 5005), add:

```c
/* One verified AL1 decode fixes the true mapping only up to the mappings whose AL1 family holds its
 * REG set. Report how many distinct families remain: 1 means the banked mapping decodes every AL1
 * candidate; >1 means AL1 coverage is partial until more evidence arrives (resolved by Task 15). */
static void al1_verify_report(const nr_pdcch_lookahead_geom_t *g, int duration, int cce, uint16_t rnti)
{
  const int span = g->freq_domain * 6;
  const nr_pdcch_al1_map_t m = {(uint8_t)g->reg_bundle_size, (uint8_t)g->interleaver_size, (uint16_t)g->shift_index};
  uint16_t rs[1][6];
  if (nr_pdcch_al1_regset(span, duration, m, cce, rs[0]) != 6)
    return;
  nr_pdcch_al1_map_t cand[NR_PDCCH_AL1_MAX_MAPS];
  int n = nr_pdcch_al1_enumerate(span, duration, cand, NR_PDCCH_AL1_MAX_MAPS);
  n = nr_pdcch_al1_narrow(span, duration, (const uint16_t (*)[6])rs, 1, cand, n);
  const int fam = nr_pdcch_al1_family_count(span, duration, cand, n);
  LOG_A(PHY, "SENSING: AL1_VERIFY rnti=0x%04x cce=%d mapping=%d/%d/%d consistent_mappings=%d distinct_al1_families=%d%s\n",
        rnti, cce, m.bundle, m.interleaver, m.shift, n, fam, fam > 1 ? " (AL1 coverage partial)" : "");
}
```

4. At the verification site, fetch the lane geometry BEFORE observing (after verification `nr_pdcch_blind_lookahead_get()` returns false). Replace

```c
        const bool just_verified = nr_pdcch_blind_lookahead_observe(lane,
            cand_task[ti].dl_raw.rnti, mono >= 0 ? (uint32_t)mono : abs_slot, cand_task[ti].dl_raw.payload);
        if (just_verified) {
```

with

```c
        nr_pdcch_lookahead_geom_t vg;
        const bool have_vg = nr_pdcch_blind_lookahead_get(lane, &vg);
        const bool just_verified = nr_pdcch_blind_lookahead_observe(lane,
            cand_task[ti].dl_raw.rnti, mono >= 0 ? (uint32_t)mono : abs_slot, cand_task[ti].dl_raw.payload);
        if (just_verified) {
          if (have_vg && vg.al1_only && cand_task[ti].L == 1)
            al1_verify_report(&vg, nr_pdcch_blind_monitor_get_cfg()->coreset_duration, cand_task[ti].cce,
                              cand_task[ti].dl_raw.rnti);
```

- [ ] **Step 5: Build everything and run the regression suites**

Run: `ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL/cmake_targets/ran_build/build && make -j12 nr-uesoftmodem test_nr_pdcch_blind_monitor test_nr_pdcch_al1_map 2>&1 | grep -E 'error|warning: unused|Built target nr-uesoftmodem' && ctest -R 'test_nr_pdcch_blind_monitor|test_nr_dl_adaptive|test_nr_pdcch_al1_map' --output-on-failure | tail -5"`
Expected: `Built target nr-uesoftmodem`, no errors, all listed tests pass. `ISAC_AL1_COVER` unset leaves `s_lane_dispatch_stage` exactly as before (stage ≥ 0), so the default path is unchanged by construction.

- [ ] **Step 6: Commit**

```bash
ssh sens6 "cd /home/sens/NICOLA/adaptive-rx-UL-DL && git add openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c openair1/PHY/NR_UE_TRANSPORT/tests/nr_pdcch_al1_map_test.cc && git commit -F -" <<'EOF'
Blind PDCCH: optional AL1 cover lap before the staged mapping walk (ISAC_AL1_COVER=1)

With the knob set, lookahead lanes first walk only the AL1 cover of each extent (2-11 mappings
instead of up to 1081), scanning AL1 only, then continue with the unchanged staged walk so AL2+
discovery is kept. An AL1 verification logs how many AL1 families stay consistent with the decoded
candidate (1 = the banked mapping is exact for AL1). Default off.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01GbAQEPru1r66mLFQ24UC2P
EOF
```

---

