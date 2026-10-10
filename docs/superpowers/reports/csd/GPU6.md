# GPU6 — CPU/GPU parity and the 0.5 ms/slot budget on the GB10 (2026-10-10)

Branch `csd/gpu3`, worktree `rr-csd-x`. Host: DGX Spark GB10 (sm_121, unified memory). Every build, test, bench and
replay ran through `heavy.sh`. No radio or OTA was used. All numbers are offline: either recorded-input benches or
raw-IQ replays of `raw_pci415_12s_b` ("b") and `raw_pci415_12s_20261008` ("old") at 0.25×, with the `ota/t26g.env`
features on host-safe cores. Measured binaries are commits `f741391930` and `15878735d3`.

## Verdict

* **Parity: met, and exact by construction.** The GPU now gives the unchanged CPU chain the same rxdataF, bit for bit.
  It also computes the CPU's channel-estimator delay bit for bit. So the LLRs, CRC and EVM are the CPU receiver's own.
  - The offline benches found **0 differing samples** in 6,324,224 FEP outputs, and **228/228** delays were equal.
  - Inside the receiver, the GPU FEP was checked against the CPU FEP on every symbol of every antenna. That covered
    **21,000 FEP calls** and **1,113,760 delay rows**, with **0 mismatches**.
  - The PDCCH CPU-arithmetic kernel is bit-exact on **26,000/26,000** passes over the three CORESETs at rb_offset 0.
    The CORESET at rb_offset 36 (nID 7684) differs in its last 36 RBs. That points to a defect on the CPU side (see
    the PDCCH section below).

  `[MEASURED, DGX GB10 bench + offline replays @15878735d3]`
* **Admission: met.** Every eligible PDSCH decode used the GPU front end: 1,000–20,000 GPU FEP calls per replay, 0
  fallbacks, 0 busy. The PDSCH LLR path from GPU2–GPU4 is no longer loaded: it is not CPU-equivalent. `[MEASURED]`
* **Timing: not met; the floor is measured below.** Per PDSCH decode, the GPU removes the FEP (858 → 84 µs on b,
  988 → 91 µs on old) and the delay IDFT (chest 1,572 → 881 µs on b, 972 → 476 µs on old). That is −44 to −46 % per
  decode. The floor is still **~1.6–2.0 ms per decode** on the shared replay cores. The dominant remaining cost is the
  CPU demodulation in `nr_dlsch_demodulation.c` (~0.8 ms), then the rest of the CPU chest and LDPC (~0.2 ms). The
  all-grants slot span p99 (47–51 ms) is queueing and hypothesis retries. The scan is unchanged at p99 1.5–3.5 ms,
  dominated by CPU post-processing and decode. `[MEASURED]`

## 1. Root cause of the stuck parity

**PDSCH.** I finished the GPU5 work-in-progress (commit `fe90794430`). Its v3 records hold the CPU rxdataF for all 14
symbols, the per-(symbol, layer, antenna) pilot LS and final estimate, the delay, the noise, and the provenance of
each grant.
- On a fresh t26g dump, every grant below 99.9 % agreement had `recovered=0 csi=0 ptrs=0`, Nl=1 QPSK. So the prime
  hypothesis is **refuted**: these were not retried-geometry, CSI-exclusion or PT-RS grants. `[MEASURED]`
- The disagreement tracks EVM: 55–70 % EVM grants agree on 83–94 % of signs, 10–20 % EVM grants on 99.5–99.9 %.
  `[MEASURED]`
- Pilot LS agrees at −50…−59 dB (−27…−34 dB on the weak antenna 2). The final per-RE estimate diverges at
  −15…−33 dB. The first diverging stage is **frequency interpolation**: the CPU uses a delay-compensated FIR (delays
  up to 76 samples); the GPU used plain linear interpolation. Noise estimates also differed by 2–7×. `[MEASURED]`

The CUDA path was a different receiver: float LS, linear interpolation, adjacent-pilot noise and whitened 4-branch
MMSE. At low SNR, two valid receivers disagree on many bits. The CPU chain is also sensitive to the last bit of its
input: CHESTFAST measured that a float delay IDFT changes the chosen delay on 297/368 inputs. No float
re-implementation can reach 99.9 % there. `[CODE-READ + MEASURED]`

**Fix.** The GPU now reproduces the CPU's input exactly, so the CPU chain is untouched:
- `dft4096()` from `oai_dfts_neon.c`, lane for lane: dft16, bfly4_16 ×2, bfly4 ×2, NEON saturating adds, the
  `vqshrn #15` packs, scale shifts 3/1/1/1, the composed `transpose16_ooff` permutation and the `init_rad4`
  twiddles.
- `apply_nr_rotation_symbol_RX()`: QRDMX rotation, then `mult_cpx_vector` by the timeshift table, on the occupied
  subcarriers only.
- `idft4096()` and `nr_passive_est_delay()` for the channel-estimator delay.

**PDCCH (88 / 77 %).** The float GPU kernel skipped `dci_nr.c`'s fixed-point steps:
- filt16 truncation;
- the roughness branch gate;
- the compensation shift;
- the P90 autoscale and the [−32, 31] clip.

The CPU's 40,612 / 76,745 zero LLRs come from that chain. GPU5's integer kernel was exact. I committed it
(`3d93912d72`) and parallelised it (`c882b56440`).

## 2. Parity results

| Check (GB10) | Input | Result |
|---|---|---|
| Old GPU LLR path, fresh t26g dump b (19 grants) | CPU LLRs | 86,320/92,184 signs (93.64 %): fail, algorithmic |
| Exact FEP vs recorded CPU rxdataF | b v3: 3,375,104 samples (824 symbol-antennas) | **0 differing** |
| | b v2: 1,343,488; old v2: 1,605,632 | **0 differing** |
| Exact delay vs CPU delay in v3 stage rows | 228 rows | **228 equal** |
| PDCCH CPU-arithmetic LLR | b 110,592; old 110,592; t26g b 442,368 | **all exact** |
| PDCCH, 30,000-pass t26g dump of b | nID 10163 / 62922 (216 RB), nID 415 (48 RB), rb_offset 0 | 26,000/26,000 passes exact |
| | nID 7684 (156 RB, rb_offset 36, symbol 1) | 0/4,000; differences in RB 120–155 |

In-receiver checks used `ISAC_PDSCH_GPU_EXACT_CHECK=2` (the CPU recomputes every GPU symbol) and
`ISAC_CHEST_GPU_DELAY_CHECK=1` (the CPU recomputes every GPU delay). Any difference would disable the path.

| Replay | FEP calls fully checked / mismatches | Delay rows checked / mismatches |
|---|---:|---:|
| b, final build | 20,000 / 0 | 1,076,068 / 0 |
| old, final build | 1,000 / 0 | 37,692 / 0 |
| b, earlier builds | 12,000 / 0 | 810,639 / 0 |
| Spot checks in the timing arms (1 antenna-symbol/call, 1/64 delays) | 20,000 / 0 | 9,631 / 0 |

**Per-RNTI CRC and EVM.** Two replays of the same capture do not reproduce the sample stream. `nr_gpu_slot_bench
--pair` found **0/16** matched grants with identical raw IQ between two runs. Cross-run per-grant comparisons
therefore measure the replay harness, not the GPU:
- The CPU-vs-CPU control on b gives 53 % identical grant outcomes and 19 CRC differences on 1,433 common grants.
- Decode yield varies widely run to run: CRC-OK on b ranges 650–2,090 for CPU arms and 463–2,102 for GPU arms.

One pair did align on old: the CPU arm against the GPU arm with exact FEP and delay. It shows **343/343 common
grants, 0 CRC differences**, and identical per-RNTI DCIRAW / CRC-OK / median SNR. 89.5 % of grant outcomes are
identical in every field; the remaining 36 differ only in chest-context fields (alpha, branch SNR, noise), the same
fields that differ CPU-vs-CPU. `[MEASURED]`

| old, RNTI | DCIRAW CPU / GPU | CRC-OK CPU / GPU | median SNR CPU / GPU |
|---|---:|---:|---:|
| `0x8f5a` | 466 / 466 | 183 / 183 | 8.0 / 8.0 dB |
| `0xffff` (SI) | — | 14 / 14 | 12.1 / 12.1 dB |

**PDCCH known issue: CPU defect, code-read hypothesis.** `dci_nr.c nr_pdcch_extract_rbs_single()` indexes the
frequencyDomainResources bitmap with `(cset_start + coreset.rb_offset)/6`. A CORESET with a non-zero
`coreset.rb_offset` therefore skips its last rb_offset/6 groups and leaves those extracted REs stale. The measured
pattern fits: the differing RBs are exactly the last 36 = 6 groups of the nID 7684 CORESET. If this holds, the CPU
loses DCIs whose CCEs fall in those RBs. This is not my file; it is for the PDCCH owner. The GPU PDCCH LLR stays
diagnostic-only.

In the receiver, `ISAC_PDCCH_GPU_FEP=1` got 222 + 127 byte-equal passes and 4 mismatches, all before the PDCCH
module quarantined itself on an RT-deadline miss. `[KNOWN ISSUE]`

## 3. Admission and timing

These are PDTIM per-decode means, wall/CPU in µs, on `heavy.sh` cores 0-4 and 8-14 shared with the whole replay. GPU
FEP wall per call, including the wait: b mean 105, p50 ≤ 64, p99 ≤ 1,024 µs; old mean 66, p50 ≤ 64, p99 ≤ 192 µs.
`[MEASURED]`

| Capture / arm | FEP | chest | demod | LDPC | Sum | Estimator call (delay part) |
|---|---:|---:|---:|---:|---:|---:|
| b, CPU | 858 | 1,572 | 1,016 | 207 | 3,653 | 130 (118) |
| b, exact FEP | 97 | 1,545 | 884 | 203 | 2,729 | 138 (126) |
| b, exact FEP + GPU delay | **84** | **881** | 798 | 196 | **1,959** | **74** (0) |
| old, CPU | 988 | 972 | 785 | 192 | 2,937 | 139 (122) |
| old, exact FEP | 66 | 928 | 751 | 180 | 1,925 | 149 (130) |
| old, exact FEP + GPU delay | **91** | **476** | 861 | 207 | **1,635** | **77** (0) |
| b, + `ISAC_PDSCH_GPU_LDPC=1` | 79 | 850 | 1,157 | 998 wall / 76 CPU | — | — |

The scan and all-grants slot span vary a lot from run to run. Scan is SCANTIME_FINE; slot span is
`ISAC_PDSCH_SLOT_TIMING`, first-to-last consumer per slot.

| Capture / arm | Scan p50 / p99 µs | Slot span p50 / p99 µs | Decode slots / jobs |
|---|---:|---:|---:|
| b, CPU | 514 / 2,536 | 10,271 / 84,025 | 3,431 / 7,597 |
| b, exact FEP + GPU delay | 613 / 3,527 | 4,110 / 46,778 | 4,756 / 10,313 |
| old, CPU | 354 / 1,430 | 3,342 / 90,861 | 444 / 598 |
| old, exact FEP + GPU delay | 342 / 1,496 | 1,533 / 51,486 | 445 / 598 |

* **Standalone GPU cost** (bench, 20 repeats): bit-exact FEP of a whole slot (4 RX × 11 symbols) takes 31.0 / 34.7 µs
  p50 / p99 zero-copy, versus 138 / 145 µs staged. One CORESET symbol (4 RX) takes 25 µs. A delay batch of up to 12
  rows takes 39 µs. `[MEASURED]`
* **Floor and dominant cost:** ~1.6–2.0 ms per decode. Demodulation is 0.8–0.9 ms. The rest of the chest is
  0.48–0.88 ms (LS, FIR, noise, the GPU wait). LDPC is ~0.2 ms. The scan p99 is CPU `post` and decode, not FEP:
  BTIM `fep_llr` is 107–120 µs mean of 255–355 µs.
* **Not recommended:**
  - `ISAC_PDCCH_GPU_EXACT_FEP=1` measured no scan gain (`fep_llr` 107 vs 120 µs mean, worse max).
  - `ISAC_PDSCH_GPU_LDPC=1` made the LDPC wall time 998 µs against 196 µs on the CPU, and its parity is unverified.
* **Next lever:** a bit-exact port of `nr_rx_pdsch` (owner: the decode files), using the same method and gates.

Robustness fixes in this round:
* A missed RT deadline used to poison the PDSCH module permanently (`CK` on the wait). It no longer does.
* In zero-copy mode, a late kernel could overwrite the CPU fallback's rxdataF. The call now waits it out and counts
  it (`late`).
* Each concurrent call gets its own stream and mapped output. Consumers never share a buffer or a lock.

## 4. OTA env for the orchestrator's on-air A/B

Binary: branch `csd/gpu3` at `6d9d802a43` (includes `15878735d3`), built with `pdsch_gpu`. Config, cores and X410
arguments are unchanged from the t26g recipe.

* **Arm A (CPU front end):** the full contents of `ota/t26g.env` followed by `ISAC_PDSCH_GPU=0`.
* **Arm B (GPU, CPU-equivalent):** `ota/t26g.env` followed by:

```text
ISAC_PDSCH_GPU=0 ISAC_PDSCH_GPU_EXACT_FEP=1 ISAC_PDSCH_GPU_EXACT_CHECK=1 ISAC_CHEST_GPU_DELAY=1
```

`ISAC_PDSCH_GPU=0` is required. `t26g.env` sets `NR_GPU_FEP=1`, and if the exact path ever failed to initialise, that
legacy alias would load the non-equivalent GPU LLR path.

Leave unset or at their defaults:
- `ISAC_PDCCH_GPU_FEP`
- `ISAC_PDCCH_GPU_EXACT_FEP`
- `ISAC_PDSCH_GPU_LDPC`
- `ISAC_CHEST_GPU_DELAY_CHECK` (default 64)
- `ISAC_PDSCH_GPU_EXACT_DEADLINE_US` (default 2000; in zero-copy mode it only counts)

Expected log lines in arm B:
- `GPU_EXACT_FEP enabled ... check=1`;
- periodic `GPU_EXACT_FEP calls=... check_bad=0 ... delay_bad=0 direct=1`;
- no `MISMATCH` line;
- PDTIM `fep` near 0.1 ms and `chest` roughly halved against arm A.

`[IMPLEMENTED, NOT VALIDATED]` over the air.

## 5. Commits, files and reproduction

* `fe90794430` test(gpu): per-stage PDSCH parity records (v3 dump, stage/provenance, `gpu5_replay.sh`).
* `3d93912d72` fix(gpu): CPU/GPU parity — PDCCH LLR in the CPU's fixed-point arithmetic.
* `bdcf33f8b6` fix(gpu): CPU/GPU parity — bit-exact port of the aarch64 NEON FEP.
* `c882b56440` perf(gpu): parallel PDCCH CPU-arithmetic LLR kernel.
* `f741391930` perf(gpu): zero-copy exact FEP, bit-exact delay IDFT, per-call streams.
* `15878735d3` perf(gpu): CPU-equivalent GPU front end in the passive PDSCH decoder.
* `6d9d802a43` test(gpu): PDCCH bench exactness per pass and per CORESET.

Main files:
- `openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_gpu_exact_fep.cuh` and `nr_pdsch_gpu_exact_tw.h`
- `nr_pdsch_gpu_fep.cu`
- `nr_pdsch_gpu_exact.{c,h}`
- `nr_pdcch_gpu_parity.cuh`
- `NR_UE_ESTIMATION/nr_dl_channel_estimation.c`
- hooks of 2–4 lines in `nr_pdsch_passive_decode.c`, `nr_pdsch_passive_queue.c` and `nr_pdcch_blind_monitor_rt.c`
- `tests/passive_rx/dgx/gpu6_replay.sh` and `gpu6_pair.py`

`ctest -j4 -E nr_td_sim_test`: 210/216 passed. The failures are the known ARM ones (`dft_test`,
`test_nr_modulation`, both `pusch_ra0` QAM cases), `test_nr_td_grantwork` (which also failed in GPU2), and
`nr_polar_sc_cuda_test` (not built by the `tests` target). The sens6 gate and `git diff --check` are clean.
`[OFFLINE VERIFIED, GB10]`

```sh
O=/home/nicola/NICOLA/wt/rr-orchestration; B=$PWD/cmake_targets/ran_build/build
$O/heavy.sh ninja -C "$B" -j6 nr-uesoftmodem pdsch_gpu pdcch_gpu nr_gpu_slot_bench nr_pdcch_slot_bench
$O/heavy.sh env LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_gpu_slot_bench "$2" --exact 20 && ./nr_gpu_slot_bench "$2" --delay 20' _ "$B" "$PWD/replays/gpu5_cpu_b.bin"
$O/heavy.sh env ISAC_PDCCH_GPU_CPU_ARITH=1 LD_LIBRARY_PATH="$B:/usr/local/lib" bash -c 'cd "$1" && ./nr_pdcch_slot_bench "$2" --gpu 20' _ "$B" "$PWD/replays/pdcch_cpu_b.bin"
bash tests/passive_rx/dgx/gpu6_replay.sh b cpu ISAC_CHEST_STAGE_TIMING=1
bash tests/passive_rx/dgx/gpu6_replay.sh b exact ISAC_CHEST_GPU_DELAY=1 ISAC_CHEST_STAGE_TIMING=1
bash tests/passive_rx/dgx/gpu6_replay.sh b exactchk ISAC_CHEST_GPU_DELAY=1 ISAC_CHEST_GPU_DELAY_CHECK=1
# same with "old"; pairing: python3 tests/passive_rx/dgx/gpu6_pair.py <A>/run.log <B>/run.log
```

## Deferred

* OTA A/B: the orchestrator runs it, using the env in §4.
* A bit-exact GPU port of `nr_rx_pdsch` (the dominant remaining cost) would need edits in the decode files I do not
  own.
* The suspected CPU PDCCH extraction defect for CORESETs with `coreset.rb_offset != 0` goes to the PDCCH owner.
