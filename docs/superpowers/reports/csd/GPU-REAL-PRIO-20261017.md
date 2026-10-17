# GPU priority for real first-pass grants, 2026-10-17

Sonnet agent; saved by the orchestrator. No code change (the mechanism already exists).
Verdict: NOT met. ISAC_PDSCH_GPU_PRIO=1 + ISAC_PDSCHQ_REAL_FIRST=1 on top of t26cE gives no tot p99 gain beyond run spread on either capture. Keep both default-off.

## What already exists [CODE-READ]
Commit 347001a16d: ISAC_PDSCH_GPU_PRIO=1 gives every PDSCH FEP/chest, demod and GPU-LDPC context a low-priority twin stream in the same green-context partition (nr_pdsch_gpu_prio.cuh); probe and retry bodies launch on it (nr_pdsch_gpu_set_lowprio(t_probe_first_seg || t_pdtim_body > 1)). ISAC_PDSCHQ_REAL_FIRST=1 puts full decodes ahead of probes. t26cE already has ISAC_PDSCHQ_PROBE_LAST=1, RT isolation and CUDA_DEVICE_MAX_CONNECTIONS=32. DECTAIL-20261012 found no gain in the old regime either. Gap: GPU-pass-graph streams have no low-priority twin.

## Results [MEASURED, DGX replay <cap>, t26cE off vs + PRIO + REAL_FIRST on, n=3, interleaved] (runs 1/2/3, us)
| cap | arm | tot p50 | tot p99 | tot p99.9 | crc_q | drops % | rec | RNTIs | n_real |
|---|---|---|---|---|---|---|---|---|---|
| b | off | 1357/1600/1507 | 3815/5792/3852 | 7201/11374/7858 | 96.7/95.7/96.9 | 2.57/0.03/0 | 25/19/22 | 18/22/31 | 1345/2063/4541 |
| b | on | 1712/1559/1507 | 5565/4575/4273 | 12038/7511/6542 | 97.5/95.6/95.7 | 0/0/0 | 25/34/21 | 27/31/26 | 1903/4812/4343 |
| tim | off | 1070/1061/1004 | 3048/2557/2766 | 6016/5534/4281 | 72.7/75.4/71.7 | 0 | 3/1/4 | 13/12/14 | 4592/5911/5198 |
| tim | on | 974/1069/1061 | 2483/2891/2639 | 5833/8125/3852 | 73.5/77.5/72.6 | 0 | 2/3/1 | 13/13/14 | 4787/5853/4443 |
Median p99: b 3852 off vs 4575 on; tim 2766 vs 2639 (overlapping).

## Why it does not help (stage_split, run 1 per arm)
Tail grants (> p99) wait inside their own chest and LDPC stages: chest gpuwait median/p90 tim 655/1043 off vs 270/586 on, b 391/1069 vs 525/1546; LDPC 242-250 / 409-836. Grants with >= 4 code blocks take 1.3-2.6 ms first pass vs ~0.5 ms for 1-3 CBs; their chest gpuwait (160-340 us) is mostly their own kernel time. Priority orders pending blocks but never preempts; remaining contention is real vs real (plus UL) in the RT partition [INFERRED, no nsys].
Next: shorten large-CB chest and LDPC kernels or split them across SMs.
