# CORESET blind-search speedup: GPU chest/LLR module built; decode stays CPU; the real lever is
# multi-candidate-per-occasion (design only, NOT implemented) — 2026-09-16

## Why this exists

`nr_pdcch_blind_monitor.c`'s CORESET blind discovery tries one (extent, mapping) hypothesis at a
time (`s_ext_idx`/`s_map_idx`), each given `NR_PDCCH_EXTENT_VERIFY_OCC = 4000` REAL occasions before
`extent_advance()`. Up to 1035 extents x 512 mappings. Measured live on a real commercial cell
(20 MHz/51 PRB, 4 antennas, `BTIM` report over 438000 occasions): `fep_llr` (chest+LLR-gen) mean
12.8 us/max 121 us; `decode` (Polar SCL + CRC) mean 130.2 us/max 302.7 us — 10x fep_llr and the
dominant per-candidate cost — with 433078/437131 (99 %) of occasions already over the 500 us slot
budget at a SINGLE active hypothesis. This is wall-clock bound (real occasion arrival rate), not
compute bound, so testing K candidates per real occasion instead of 1 is the fix, not raw FLOPS.

Standing constraint (user rule, do not relax): never seed the search from SIB1/known-cell
configuration, never shorten `NR_PDCCH_EXTENT_VERIFY_OCC` or any other per-candidate occasion budget
to buy convergence speed.

## What was built this session

`openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_gpu_fep.{h,cu}` + `tests/nr_pdcch_gpu_fep_test.cc`, CMake
wiring in `openair1/PHY/CODING/CMakeLists.txt` (mirrors the existing `pdsch_gpu` target exactly,
same `NR_GPU_FEP=1` loader knob, builds `libpdcch_gpu.so`). A GPU-batched DM-RS LS channel estimate
+ QPSK LLR generator for N independent CORESET-extent candidates sharing one resident,
already-FFT'd slot — mirrors `nr_pdsch_gpu_fep.{h,cu}`'s dedicated-worker-thread / dlopen'd-module
architecture (the "one GPU worker, not a per-consumer mutex" lesson from commit `002038bd70` is
reused, not re-earned). Math mirrored from `nr_dl_channel_estimation.c`'s
`nr_pdcch_channel_estimation()` (non-CH_INTERP branch) and `dci_nr.c`'s `nr_gold_pdcch()` /
`nr_pdcch_dmrs_ref()` / `nr_rx_pdcch_symbol()`, which were read in full for this.

**Known, documented simplification**: multi-antenna combining is plain power-weighted MRC, not the
branch-roughness gate `nr_rx_pdcch_symbol()` grew on 2026-09-15 (zeroing branches whose
adjacent-subcarrier estimate roughness exceeds 4x the smoothest). That heuristic was tuned against
one X410 4-RX capture; porting it to CUDA without hardware to re-verify against would be guessing,
not mirroring. Flagged in the header, not silently dropped.

**Validated**: the header compiles standalone as C11; the test file compiles standalone as C++17
(`g++ -std=c++17 -c nr_pdcch_gpu_fep_test.cc`, clean) — this exercises every struct field and API
call site for type/signature agreement. A real numeric bug WAS caught this way while writing it (the
host-side upload staging buffer was sized for `int16` count but reinterpreted as `float2`, half the
needed bytes — fixed) and a second real bug (the DM-RS c_init's absolute symbol number `l` was
conflated with the resident buffer's CORESET-relative row index `s` — they differ by `start_symbol`
and must not share one variable — fixed, see the .cu's `upload_slot()` comment).

**NOT validated, and CANNOT be from this environment**: no CUDA toolkit and no NVIDIA GPU were
available here (checked: no `nvcc`, no `/usr/local/cuda`, no `nvidia-smi`; this repo's own
`[[gpu-ldpc-on-sens6]]` memory confirms only sens6 has the GPU). The `.cu` file's kernels
(`__global__` code, `<<<...>>>` launches) were never compiled or run. **Before trusting this module
live**: build on sens6 (same CUDA 12.4 + g++-13 / sm_89 toolchain as `libldpc_cuda.so`), run
`nr_pdcch_gpu_fep_test` (checks internal consistency of a synthetic noiseless occasion — chest
recovers h≈1, LLR signs agree — but does NOT check the true bit-sign convention against
`nr_pdcch_demapping_deinterleaving()`), then wire in a self-check comparing GPU vs. CPU LLRs on a
handful of real candidates from a live capture (mirror `nr_pdsch_gpu_fep`'s `ISAC_GPU_SELFCHECK`
pattern) and gate live use on that agreeing.

## What was deliberately NOT built, and why

**Polar SCL decode + CRC was NOT ported to GPU.** Three independent reasons, not one:
1. `blind_polar_decode()` → `polar_decoder_int16()` is 822 lines
   (`nrPolar_tools/nr_polar_decoder.c`) plus 432 lines of decoding tools plus a 36304-line
   precomputed matrix table — a faithful port is a multi-session undertaking on its own, not
   something to attempt unverified in the same pass as the chest/LLR module.
2. **Direct precedent in this codebase says a naive GPU port of a channel decoder LOSES badly**:
   `[[gpu-ldpc-on-sens6]]` measured the GPU LDPC decoder at 20x slower wall time and 6x more CPU
   than the CPU decoder in the live receiver, root-caused to synchronous blocking submission
   (consumers submit one TB and block, so the batcher never sees more than ~1 TB) — a redesign
   (async submission + real batching), not a parameter, was needed and has not yet been done even
   for LDPC. Polar SC/SCL decoding has MORE serial dependency per bit than LDPC's per-check-node
   updates (a genuinely sequential decode tree, list-sorted at each stage), making it a WORSE GPU
   candidate for a short block code like DCI, not a better one.
3. This environment cannot build or run CUDA at all, so an unverified from-scratch port of a
   1250+-line table-driven decoder would be exactly the kind of "confident and untested" numerical
   code this project's own culture (`[[verify-before-asserting-never-inherit-a-number]]`, and the
   long list of retracted claims in CLAUDE.md) explicitly warns against shipping.

**Also confirmed while reading `nr_pdcch_blind_monitor_rt.c`: CPU thread-pool fan-out of exactly
this kind of independent-candidate decode work was already tried and MEASURED WORSE** (see that
file's comment above `nr_pdcch_blind_cand_worker_serial`, ~line 2500): fanning candidate decodes out
across the shared RT-priority `Tpool` cost 271.1 us vs. 18.4 us in-line (15x) because the consumer
queues behind the shared pool at priority 50 — "the whole machine is at ~1.5 of 12 cores, so there
is no throughput reason to fan out — only a latency one, and latency is exactly what the deferral
already bought." This is strong, already-measured evidence against a lazy "just use a CPU thread
pool for decode" alternative too, UNLESS it uses a dedicated pool separate from the shared RT one
(see below).

## The actual recommended fix — NOT implemented this session, here is the concrete design

**Multi-candidate-per-occasion testing on the EXISTING, unmodified, already-verified CPU decode
path.** This is architecturally the highest-leverage, lowest-risk lever, independent of GPU:

- `nr_slot_fep()` (the FFT/CP-removal stage) already computes the WHOLE carrier's spectrum per
  symbol regardless of which CORESET RB range is being tested — it is NOT re-run per candidate
  today and was never the measured bottleneck (12.8 us mean already includes chest+LLR, not just
  FEP). Testing K different CORESET-extent candidates against the SAME occasion's already-computed
  FFT output costs the FEP exactly once, however large K is.
- Change `s_ext_idx`/`g_cfg.coreset_rb_offset`/`g_cfg.coreset_freq_domain` (currently one global
  active hypothesis, `nr_pdcch_blind_monitor.c` ~line 330-460) from a single index into a small
  in-flight WINDOW of `K` extent candidates (`NR_PDCCH_EXTENT_BATCH`, e.g. 8-32, env-tunable), each
  tracked with its OWN occasion counter (`s_ext_occ[k]`) and its own evidence table
  (`s_ext_evidence[k][...]`), advancing to the next untried extent independently when its own
  `NR_PDCCH_EXTENT_VERIFY_OCC` budget is exhausted — so NO candidate's occasion budget shrinks, only
  the number of candidates tried per unit of real time grows by ~K.
- In `nr_pdcch_blind_monitor_rt.c`'s decode call site (~line 1468 for the single-hypothesis
  `rel15->coreset` build, ~line 2420 for the analogous pbwp-probe block that already loops over
  multiple hypotheses per occasion for a DIFFERENT dimension — a working precedent for exactly this
  kind of per-occasion inner loop already exists in this file), build K `rel15->coreset` configs
  from the K live extent candidates and run the existing `nr_pdcch_generate_llr` →
  `nr_pdcch_demapping_deinterleaving` → `nr_pdcch_unscrambling` → `blind_polar_decode` chain K times,
  reusing the ONE `nr_slot_fep()` call already made for that occasion. Each candidate decode stays
  IN-LINE (serial, on whatever thread already owns this occasion) — exactly the pattern the
  already-measured comment above endorses, not the rejected shared-Tpool fan-out.
- `nr_pdcch_gpu_fep`'s batched chest/LLR (this session's deliverable) becomes useful headroom once K
  grows large enough that even the (cheap) chest/LLR stage starts to matter — it does not need to be
  wired in before the CPU multi-candidate change lands; the two are independent, additive levers.

**This was not implemented this session** because: (a) `nr_pdcch_blind_monitor.c` +
`nr_pdcch_blind_monitor_rt.c` together are 2000+ dense, heavily-instrumented, already-mature lines
(RNTI persistence rings, BWP-tracking state, evidence tables, many prior hard-won fixes documented
inline) that this session did not have time to read in full; (b) this environment has **no build
access to this worktree at all** (`ninja`/build-directory access is blocked by a sandbox deny rule —
confirmed, matching the earlier fork's own report of the same constraint) so the change could not be
compiled or tested; and (c) a global-state restructuring of a hot path this dense, landed unverified,
is a worse outcome than a clear, concrete, actionable design note for whoever has build access next.

## Summary for whoever picks this up

1. `nr_pdcch_gpu_fep.{h,cu}` — build + test on sens6, run the self-check gate, THEN wire into the
   RT decode path (only worth doing once #2 exists, since it isn't the bottleneck at K=1).
2. Multi-candidate-per-occasion on CPU (design above) — the real fix for the wall-clock problem,
   buildable and testable independent of any GPU, needs careful surgery in a dense hot path with
   real compile/test access.
3. Decode/CRC stays on CPU, in-line per candidate — do not fan it out across the shared Tpool
   (measured 15x regression, see above), and do not attempt a from-scratch GPU Polar port without
   either much more time or a strong reason to believe it will not repeat the LDPC precedent.
