# Dedicated-BWP-size audit (Task 2)

**Conclusion B — already handled.** `nr_passive_bwp.c`/`.h` (`openair1/PHY/NR_UE_TRANSPORT/`, built
2026-09-15, commits `258b57702d..fce9af1de8` per the project's own memory record) already resolves an
UNKNOWN dedicated BWP size used for RIV interpretation, from first principles, with no RRC visibility:
a DCI 1_1 length `L != L0` decoded against an RNTI already proven by an accepted DCI is treated as
evidence of a new BWP; `(d, size, start)` is then resolved from per-PRB PDSCH DM-RS coherence
(`nr_pbwp_score_grant`, matched-filter sum over each candidate's claimed PRBs); TB-CRC feedback
(`nr_pbwp_feed_crc`) un-resolves a wrong hypothesis after 32 straight fails and scoring restarts. This
is exactly the "genuinely missing" mechanism Task 3 proposed to build — it exists, live, already.

**Wiring confirmed live** in `nr_pdcch_blind_monitor_rt.c`: `pbwp_snap[bi].size` (populated from
`g_pbwp.e[bi].size`, only valid once `nr_pbwp_resolved()`, guarded by `pbwp_snap[bi].start < 0` skip
checks at both call sites) feeds `cand_task[nof_tasks].bwp_size` directly at lines 4258 and 4367 — the
exact `cfg->bwp_size` substitution point Task 3's step 4 would have needed to locate is already done,
by this module, not by a hardcoded `cfg->bwp_size = n_rb_carrier` value.

**Step 1 finding**: the `pbwp_snap`/`pbwp_on`/`pbwp_n`/`pbwp_probe_*` locals in
`nr_pdcch_blind_monitor_rt.c` are a per-slot snapshot copied out of the persistent `g_pbwp` state
(`nr_passive_bwp.h`'s `nr_pbwp_t`) for the RT loop to build candidate decode tasks from. They are not
themselves the discovery mechanism — `nr_passive_bwp.c` is — but they ARE how a resolved dedicated-BWP
size reaches the live per-occasion decode path, both in the initial candidate build (line ~4253-4268)
and a second, CORESET-gated pass (line ~4362-4376). `pbwp_probe_len`/`nr_pbwp_next_probe_len` is the
round-robin unresolved-length prober (`s_probe_tick & 3`, one occasion in four) that feeds new
candidate lengths into scoring before a size is known at all.

**Step 2 finding**: the memory file (`passive-bwp-tracking-and-phy-test-bed.md`, full text read, not
just its truncated index line) describes this as a SOLVED problem: "17/17" gtests at write time (now
17 Pbwp + 4 other = 21/21, re-run live this session, all pass), with one documented, understood, and
accepted scope limit — a "collision limit": a dedicated BWP whose FDRA+indicator width happens to equal
the base BWP's arrives at the same DCI length and is undiscoverable by this length-based method (the
project's own rfsim test conf sidesteps it by picking a BWP size that avoids the collision). This is a
known, documented gap in an otherwise-solved mechanism, not evidence the mechanism is missing.

**Step 3 finding**: `grep -n 'BWPSize\|bwp_size' nr_pdsch_config_sweep.c` returns **nothing** —
confirms the header's own claim that BWP size sits entirely outside Technique D's scope (TDA/DM-RS
position/DM-RS length/MCS table only, assumes geometry already resolved). This division of labor is
intentional and not a gap at the seam: `nr_passive_bwp` resolves the geometry (size/start/indicator
width) before `nr_pdsch_config_sweep` ever runs on grants from that BWP.

**Conclusion: Task 3 is cancelled.** No new `nr_hyp_sweep`-based BWP-size module is needed — building
one would duplicate `nr_passive_bwp.c`'s already-live, already-tested, already-wired mechanism. Proceed
directly to Task 4 (GF(2) algebraic n_RNTI recovery).

**Third time this exact mistake pattern has surfaced this session** (see the "percentage of
implementation" retraction earlier: MCS table and DM-RS pattern sweeping were also wrongly assumed
missing before being found already built as `nr_pdsch_config_sweep.c`'s Technique D). The audit-first
task ordering this plan used for Task 2 is what caught it before any code was written this time,
instead of after — worth keeping as the default pattern for any future "is X missing" question on this
codebase before writing new code for X.
