# R14 — PROJECT_MEMORY reconfiguration-robustness documentation

Branch: `rr/w5-docs`; base/integration commit: `c6fc03be7c`.

## Changed sections

- **§10.2:** documents all robustness runtime knobs and defaults: epoch enablement, length/re-lock and wide-probe controls, discovery, gap/SI/SIB1 monitoring, SIB1-less arm, and UeContext writer/test hook.
- **§11.6–§11.13:** adds DCI RELOCK/unresolved, CORESET bank lifecycle, wide probes, epoch/SI/SIB1 semantics, CSI-RS map-change meaning, UeContext records/health, and all epoch-drop metric keys.
- **§16:** adds the robustness status row and replaces the stale “no unified epoch” blocker with the experimental, unvalidated live path.
- **§18 / §23.6 / §24:** removes superseded “bank not cleared” state, and marks K10, K11, and K37 fixed with commit/evidence boundaries.
- **§25:** adds the dated `2026-10-04` architecture/operation/evidence block, 4-RX flag-off/on/SIB1-less results, K45 and R2 wide-sweep regression notes, outstanding validation, and the `Live SA bed on sens6 (R13)` runbook stub.

## Evidence recorded

- [OFFLINE VERIFIED, DGX aarch64] `c6fc03be7c` replay/integration evidence: eight replay arms, zero stale-ticket credit; R10b/RE establish class-aware consumers and targeted re-open.
- [MEASURED, DGX rfsim 4 RX, n=2] Flag off passed 2/2 (`drop_full` 0.94/1.13 %); `ISAC_RECONF=1` passed 2/2 (1.08/0.91 %); post-convergence CRC was 100 %, ttc tda0 18–25 s and tda2 41–49 s.
- [MEASURED, DGX rfsim 4 RX, n=2] SIB1-less arm passed 1/2; `drop_full` was 2.06/3.42 %, with the failed run rejected only by that pre-existing K45 gate.
- [MEASURED, DGX rfsim 106 PRB 1 RX, R2d] The unbounded 30..140 default caused 1.7–3.7 % drops versus 0.04 % pre-R2; R2d bounds exhausted scans to 30..63 plus a <=5% low-duty wide probe.

## Validation

- [CODE-READ] Documentation-only task. No build or test was run, as requested.
- [CODE-READ] `git diff --check` passed.

## Open work

- [PLANNED] R13 live SA/SIB1-less bed, OTA with `ISAC_RECONF=1`, live reconfiguration recovery, and 60-minute soak.
- [PLANNED] R18 steps 2–4 and 7 CSI-RS bed/value work. The later SA-bed task will fill `tests/passive_rx/sa_bed/RUNBOOK.md`.
