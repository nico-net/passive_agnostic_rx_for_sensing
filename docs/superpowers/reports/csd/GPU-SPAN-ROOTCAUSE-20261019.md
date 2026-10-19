# DL GPU span: root cause (p50 and p99), 2026-10-19

Status: **NOT PROVEN YET for the merge19 build.** There is no on-air profile of csd/int16-merge19. The measurement commit `d2dcce42d1` and the run spec below fix that. This report states what the existing on-air data already shows, what it rules out, and what the next run will settle.

Labels: [MEASURED source, run, date] = read from the named on-air data. [BENCH] = synthetic unit test on the DGX GPU with no receiver running (not on air; it never serves as proof). [INFERRED] = reasoning, not measured. Cells are never pooled. All values are in µs.

## 0. Data used

| tag | build | what | GPU layout |
|---|---|---|---|
| mmTM1 = ab_mmTM1_1_153134 (TIM), mmVM2 = ab_mmVM2_1_155023 (Vodafone) | merge19 d79b8b7bc5, frozen env | run logs; EVMGRANT gp= | current (DL 32 SM, probe 8, disc 8) |
| kTZ1 = nsys_kTZ1 + ab_kTZ1_1_194746 (TIM, 2026-10-18 19:47) | frozen predecessor csd/int16-int 26b16e3671, **no in-graph gate** | nsys, CUDA + graph-node trace, window 60–90 s, 19 667 passes | old (DL pass in its own RT green context, standalone demod/LDPC/FEP contexts beside it) |
| jjpAD = nsys_jjpAD + ab_jjpAD_1_023106 (2026-10-19 02:31, night) | A+D build (in-graph gate = the merge19 graph structure) | nsys, first 60 s, 3 528 passes, **mostly probes** (345 real grants in the whole 90 s run) | probe passes on the 8-SM probe context |

Tools: `tests/passive_rx/gpuspan/passes.py` and `passrep.py`. They rebuild every pass from the trace: the three `cudaGraphLaunch` of one passivePdsch thread, the conditional-body kernels attributed through their `graphNodeId`, and every other kernel classified by the thread that issued it. `gqrep.py` reads the new EVMGRANT fields.

## 1. What a DL grant looks like on air (merge19) [MEASURED mmTM1, mmVM2, 2026-10-19]

| run | real grants on the pass | gspan p50 | the p50 grant (gspan p45–p55) | the p99 grant (gspan ≥ p99) |
|---|---|---|---|---|
| mmTM1 TIM | 7 857 | 438 | C (code blocks, from TBS) p50 **1**, 16 RB, CRC-ok 38 % | gspan p50 4 615, C p50 18 (p90 31), 201 RB, CRC-ok 15 %, qwait p50 164 |
| mmVM2 Vodafone | 24 420 | 427 | C p50 **1**, 16 RB, CRC-ok 51 % | gspan p50 4 100, C p50 12 (p90 20), 185 RB, CRC-ok 33 %, qwait p50 398 |

gspan by code-block count (CRC ok / fail), p50: TIM C=1 355 / 395, C~8 807 / 630, C~16 1 386 / 952. Vodafone C=1 342 / 383, C~8 801 / 805, C~16 1 398 / 1 334 [MEASURED mmTM1, mmVM2].
- The p50 grant is a **single-code-block, 16-RB grant**, and its GPU span is still about 350–440 µs. So the p50 span is mostly cost that does not depend on grant size. It is not decode work [MEASURED, same runs].
- At a fixed code-block count, a CRC failure does not raise the p50 gspan (Vodafone C~8: 805 fail vs 801 ok). The size of the grant drives the span, not failing blocks [MEASURED mmVM2]. At C~8–16, p99/p50 is 3–8×. That spread comes from something other than the grant's own work [MEASURED; the cause is the open item §4].
- The EVMGRANT `cb=` field was not used. C comes from TBS as in TS 38.212 5.2.2. The base-graph rule is simplified (BG1 when TBS > 3824), so C is approximate for mid-size TBs [INFERRED].

## 2. Per-pass reconstruction of the gated graph structure [MEASURED jjpAD, 2026-10-19 02:31]

These passes have the merge19 graph structure: C = FEP + chest; D = chest post → k_gate_d → IF{demod, descramble, LLR post}; L = k_gate_l → IF{rate dematch, LDPC, CRC, pick, CRC}. The data comes from the probe context (8 SMs), mostly probes, 1-code-block class, at night.

Typical pass (span p45–p55, n = 357), p50 values:

| segment | µs | what the GPU did for this pass | what else ran on the GPU then |
|---|---|---|---|
| C sub-graph | 142 | k_fep_exact 89, k_delay_exact 29, k_fir 8, k_ls 2, memcpy 1 | CSI-RS discovery (its own context) |
| C→D boundary | 5.4 | nothing of its own (graph-to-graph launch on the stream) | — |
| chest_post + k_gate_d | ~20 | 13 + 7 | |
| **IF(D) body launch** (gate end → first body kernel) | **33** (p10 32, p90 46) | **nothing of its own**: the device-side launch of the conditional body | discovery only (GPU otherwise idle for this pass) |
| D body | 73 | demod 10 kernels, unscr, LLR stats/norm | |
| D→L boundary | 6.8 | nothing | |
| **IF(L) body launch** | **16.5** (p10 16, p90 21) | **nothing of its own** | |
| L body | 99 | k_rdm 9, **k_ldpc4_t 73** (1 CB, cluster of 8 blocks), CRC 11 + 1, pick 3 | |
| **span** | **402** | own kernel busy 326, own idle gaps 77 | |

- The two conditional nodes add **≈ 50 µs per pass (12 %)** in which the pass runs nothing. The latency has a narrow spread (IF-D p10 32 / p90 46 µs), which points to a fixed launch cost, not contention [MEASURED jjpAD].
- In the probe context, k_fep_exact (grid 14) takes 88–89 µs every time (p10 87.7). The same kernel, standalone in the 32-SM context, runs at p10 37 µs [MEASURED jjpAD]. 14 blocks on 8 SMs need two waves [INFERRED]. So a probe's C is about 2.4× a real grant's C: an SM-count effect of the probe partition.
- No conditional-node gaps exist in kTZ1, the build without the gate: C→D 5.4 µs and D→L 2.7 µs [MEASURED kTZ1].

## 3. Kernel stretch under concurrency [MEASURED kTZ1, 2026-10-18; old layout]

In kTZ1 the typical pass (span 776 µs under nsys) is **91 % own-kernel busy (705 µs)**. Own idle gaps are only 54 µs, and the GPU was busy with other work during them. The span is therefore the duration of the pass's own kernels. Those kernels are slower than solo by a factor that depends on **who runs at the same time**. The ratio is duration / solo p50 of the same kernel and grid:

| overlapping work (> 20 % of the kernel) | stretch p50 | share of own-kernel time |
|---|---|---|
| CSI-RS discovery only (separate SM context) | 1.03 | 8 % |
| other DL pass instances | 1.33 | 2 % |
| discovery + other DL passes | 1.47 | 15 % |
| discovery + other DL passes + DL non-pass (standalone FEP/LDPC) | 2.92 | 27 % |
| … + UL | 6.44 | 20 % |
| none | 1.00 | 2 % |

Same kernel and grid, solo vs shared, p50: k_ldpc4_t grid 8 73 → 134; k_fep_exact grid 14 53 → 96; k_mmse3_prep 24 → 54 [MEASURED kTZ1].
- **Ruled out as the p50 driver in that layout:** discovery alone. Its own SM partition stretches DL kernels by only 3 %, which agrees with the user's A/B "CSI-RS discovery off did not change DL p50".
- In the old layout, the stretch comes from RT-partition kernels running at the same time: other DL instances, standalone DL stages, UL. The p99 passes in kTZ1 (span ≥ 9.2 ms) are 9.3 ms busy plus 3.5 ms of gaps filled by other DL passes, non-pass DL and UL. That is SM sharing, not idle time [MEASURED kTZ1].
- This layout is not merge19's, where UL streams sit on the DL SMs at low priority. Whether the same stretch holds in merge19 is **not measured** [INFERRED that it does; unproven].

## 4. LDPC on the GPU path

- The kernels decode **every code block of the launch to its own stop rule, independently**. There is no abort after the first failing block. The OAI abort rule is applied afterwards by k_pick, on verdicts that are already computed [CODE: nr_pdsch_gpu_ldpc.cu k_ldpc4_t, nr_gpu_pass_graph.cu k_pick]. A failing block runs to the iteration limit, 7 snapshots with it_max 8. The kernel ends when its slowest block ends, so **one failing block sets the LDPC time of the whole TB** [BENCH, GpuPassStamps: C=5 TB corrupted from block 1, blocks 1–4 at the limit 270–283 µs, block 0 converged 118–122 µs, kernel span = the slowest block]. On air this is **not yet measured**. The new `li=` field measures it per grant.
- The launch also decodes the class's **padding blocks** (class = next power of two ≥ C) [CODE: prepare_tb, ncb_class]. In the bench, the time of one block grows with the number of blocks launched together: 1 snapshot takes 57 µs with 8 blocks, 90 µs with 16, 125 µs with 32, and blocks start together (swait < 1 µs). This points to a memory/L2 throughput limit, not to waiting for SMs [BENCH; INFERRED cause]. On air it is unmeasured: `li` reports npad, npad_last and dpad_max.
- On air, at a fixed C, CRC fail vs ok makes no p50 difference (§1). So the iteration count of failing blocks is **not** the p50 cause. It may matter at p99 (large C, 60–80 % fail) [MEASURED mmVM2 for p50; p99 open].

## 5. Conclusions so far

| claim | status |
|---|---|
| The p50 DL grant is a 1-code-block grant whose gspan (~430 µs) is fixed cost: full-slot FEP + chest (C), chest post + gate + 2 conditional-node body launches + demod, LDPC minimum + CRC | [MEASURED mmTM1/mmVM2 for the grant profile; the per-segment split is measured only on the A+D build in the probe context (jjpAD)] |
| The two IF nodes cost ~50 µs per pass with the pass idle | [MEASURED jjpAD]; on merge19 on air: **pending** (`li` d2l/g2l + nsys) |
| The span is own-kernel time stretched by RT-partition kernels running at the same time, not idle gaps | [MEASURED kTZ1, old layout]; merge19: **pending** |
| Discovery (own partition) is not the driver | [MEASURED kTZ1 stretch 1.03; user A/B 2026-10-19] |
| One failing CB holds the whole TB's LDPC kernel | [BENCH]; on air: **pending** (`li`) |
| p99 = large grants (C 12–31, 185–201 RB) + concurrency stretch + queueing behind them | grant profile [MEASURED mmTM1/mmVM2]; mechanism and queue attribution: **pending** |

No fix is proposed: no cause is proven on merge19 on air yet.

## 6. Instrumentation (commit d2dcce42d1 on csd/int16-merge19; measurement only, always on, no env flag)

- `EVMGRANT … gp=… gq=C,D,L,seq`: event-timed sub-graph spans (C+D+L = gspan), and the pass number. The same number is the payload of an NVTX mark `gp` emitted just before the launches, so an nsys pass joins its log line exactly.
- `li=valid,ncb,nconv,nlast,it_conv_max,it_conv_sum,kspan,swait,dconv_max,dlast_max,g2l,d2l,npad,npad_last,dpad_max`: per-code-block verdicts and `%globaltimer` stamps of the LDPC launch, plus the gate kernels' stamps. g2l = IF(L) launch + rate dematch; d2l = IF(D) launch + D body + D→L boundary. DIRECT model only (the DGX).
- Cost: two `cudaEventRecord` and one NVTX call per pass on the host; four 8-byte stores per gate and two per code block on the GPU.
- Test: `GpuPassStamps.SplitSpansAndLdpcVerdicts` passes in both memory models. `ctest -I 234,250`: all pass except test_nr_modulation (known ARM failure). The build has three pre-existing link failures in targets this change does not touch: test_nr_ssb_rate_match_prod, test_nr_csi_rb_extent, nr_pusch_ra0sim (undefined nr_srs_passive_enabled / nr_pusch_grant_book_* in nr_pdcch_blind_monitor_rt.c).

## 7. Next on-air run (spec) and analysis

The script is `/home/nicola/NICOLA/wt/rr-orchestration/ota/run_gspan_20261019.sh`. Run it in daytime. It uses build_t26 of /home/nicola/NICOLA/wt/rr-csd-int16mg, FROZEN_20261018.env and sd_t26ul, with no env additions. Each run is 150 s:
gsT1 (TIM, unprofiled), gsV1 (Vodafone, unprofiled), gsTN (TIM, nsys), gsVN (Vodafone, nsys).
The nsys runs use `nsys profile -t cuda,nvtx --cuda-graph-trace=node --sample=none --cpuctxsw=none -d 35`. Report generation then ends before the stop. The jjq/jjr reports were still being generated when the receiver was stopped (status 143) [MEASURED their run.log].

Analysis, ready and dry-run on a profiled unit test:
1. `gqrep.py ab_gsT1*/run.log`, then the same for gsV1: per-grant C/D/L, the L split (g2l + LDPC + after), LDPC by CRC and limit blocks, one-failing-block test.
2. `nsys export --type sqlite` (through heavy.sh), then `passes.py <sqlite> --out x.pkl`, then `passrep.py x.pkl --log <run.log> --select real` (and `--select crcfail`). Output: per-grant timelines (kernels, gaps, IF latencies, what else ran in each gap and kernel) and the queue section (who is ahead on the same thread from the slot group, and how many other decode threads were inside GPU passes during the qwait).

## 8. Unproven

The merge19 per-segment split on the 32-SM DL context. Whether the concurrency stretch persists in the merge19 partition layout. LDPC iterations and the effect of a failing block, and of padding blocks, on air. Who the p99 grants wait behind.
