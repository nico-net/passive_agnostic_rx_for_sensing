# GRIDCARRIER 2026-10-13: receiver N_RB = carrier bandwidth (n78, 30 kHz)

Branch csd/grid-carrier (from csd/otatest db826ff278). Host DGX GB10 aarch64, build_t26 (CUDA), replays 0.25x, 4 RX, `ENVFILE=t26z.env`, one run per arm
(indicative, not averaged). Labels: [MEASURED replay], [CODE-READ], [NOT RUN]. No OTA done.

## Findings
- [CODE-READ] Supported 30 kHz sizes = those with a sample rate in `get_samplerate_and_bw`: 51 (30.72 MS/s), 106/133/162 (61.44), 189/217/273 (122.88). FFT size,
  first_carrier_offset, CP lengths follow N_RB generically (`set_scs_parameters`); no hard-coded 217 found in the sizing paths I grepped (nr_ue/PHY/MAC_UE).
  Other N_RB (e.g. 100, 120) assert in `get_samplerate_and_bw`: SIB1 widths must be mapped to the next supported size (retune_args.py `auto`).
- --ssb arithmetic [MEASURED]: grid start = -C - N_RB*6 sc; --ssb = (SSB first sc - grid start) in sc. Verified on TIM 923 (Point A reported at grid sc 336 and 576 for
  the two 273 grids = predicted) and on 415 (CORESET#0 moves by +28 RB = (273-217)/2).
- The earlier wider-grid failure (RB=273 --ssb 1507, PBCH "polar decoding wrong") does NOT reproduce: 415 with 273/1507 syncs (PCI 415, MIB k_ssb=14, SIB1 CELL), with
  FO -4846 and with the default -4800. The 80 "polar decoding wrong" lines are the normal search noise (also in the 217 control). Likely cause was the older build or a
  mismatched -C/capture-centre in that run; unverified [HYPOTHESIS].
- Replay can only serve rates equal to the capture (122.88 MS/s): new `ISAC_RAW_IQ_DECIM=2|4` (default 1, replay only, `radio/COMMON/raw_iq.c`) low-pass (127-tap
  Blackman sinc) + decimates so -r 162/133/106/51 can be replayed from the 122.88 captures. It is slow (single thread, ~1/3 real-time at the 0.25x pacing):
  the 150 s timeout cut those runs before EOF (exit 124), after sync/MIB/SIB1.
- Limit [MEASURED]: a cell whose SIB1 has no servingCellConfigCommon (415, NSA) gives `carrier=unchecked` on a grid wider than the carrier (the RS-evidence path assumes
  Point A = grid start), so CORESET discovery/bank never starts (bank adds 0 vs 4 at 217). Cells with SCC in SIB1 (923, 508, 403 expected) confirm Point A from SIB1.
  For NSA cells use the exact carrier-sized grid (autofront triple).

## Results per -r [MEASURED replay]
| capture | -r / -C / --ssb | sync | MIB | SIB1 | carrier verdict | CORESET bank adds | PDSCHQ crc_ok (final line) |
|---|---|---|---|---|---|---|---|
| 415 (217 PRB carrier) | 217 / 3680010000 / 1171 (control) | PCI 415 | k_ssb 14 | yes | CONFIRMED w/o SIB1 info | 4 | 1866/10295 (18.2 %) |
| 415 | 273 / 3680010000 / 1507 | PCI 415 | yes | yes | unchecked (NSA SIB1) | 0 | 220/251 (88 %, CSS0 only) |
| 415 | 273, FO -4800 | yes | yes | yes | unchecked | 0 | 212/242 (87.6 %) |
| 415, partial carrier | 162 / 3679830000 / 847 (decim 2) | yes | yes | yes | n/a | 0 | 106/122 (87.6 %, run cut) |
| 415 | 133 / 3680010000 / 667 (decim 2) | yes | yes | yes | n/a | 0 | 106/120 (88.3 %) |
| 415 | 106 / 3679830000 / 511 (decim 2) | yes | yes | yes | n/a | 0 | 116/132 (87.9 %) |
| 415 | 51 / 3678570000 / 223 (decim 4) | yes | yes | yes | n/a | 0 | 187/212 (88.2 %) |
| TIM 923 (217) | 217 / 3759990000 / 233, `ISAC_CARRIER_FROM_SIB1=1` | PCI 923 | yes | yes | CONFIRMED (SIB1) | 1 (len 47) | 16/837 (2.0 %) |
| TIM | 273 / 3759990000 / 569 | yes | yes | yes | MISMATCH (wider, Point A sc 336), contained | 1 (same nID 1931, rnti 0x4e32, grid_rb 28) | 18/916 (2.0 %) |
| TIM | 273 / 3752790000 / 809 (re-centred -7.2 MHz) | yes | yes | yes | Point A sc 576, contained | 1 (same, grid_rb 48) | 17/800 (2.1 %) |
162/133/106/51 on 415 are sub-carrier grids (carrier 217): they test start-up geometry/SSB/CORESET#0 only; CORESET discovery/PDSCH of the full carrier is not claimed.
TIM PDSCH 2 % is the known replay level of this capture (3.6-3.8 % in CARRIERGEOM), unchanged by the grid size.
[NOT RUN] a capture of a genuinely 162/51 PRB cell (508, 403): none exists; the first one is the OTA below.

## Restart path
`tests/passive_rx/autofront/retune_restart.sh RUN.LOG [auto|N]` (orchestrator-run): detects ISAC_ACQ_RETUNE / carrier MISMATCH in the first run's log, calls
`survey/retune_args.py RUN.LOG auto` (new `auto` = smallest supported size >= SIB1 carrierBandwidth, grid starts at Point A when the size is supported) and prints
`-C <hz> -r <n> --ssb <sc>`; with `PID=<pid>` it SIGINTs the old receiver (never SIGKILL) and waits 60 s. Exit 3 = geometry already matches, do not restart.
Checked on ab_P508 log: auto -> `-C 3590010000 -r 162 --ssb 193`; explicit 217 -> `-C 3590190000 -r 217 --ssb 517` (previous recipe), 273 -> `-C 3590190000 -r 273 --ssb 853`.
Use: run_once with generous 217 grid -> retune_restart.sh -> relaunch with the printed args (+ `ISAC_CARRIER_FROM_SIB1=1` for any grid wider than the carrier).

## OTA spec lines (orchestrator; TAG:ENV:CFG:FREQ:SSB:FO; extra receiver args replace the usual `-r 217`)
```
G508:t26z+ISAC_CARRIER_FROM_SIB1=1:<cfg of ab_U508a>:3590010000:193:-4842    # receiver args: -r 162 (centre 3590.01 MHz, grid = carrier 3560.85-3619.17)
G403:t26z+ISAC_CARRIER_FROM_SIB1=1:<cfg>:<C from retune_restart.sh on the first 403 run>:<SSB>:<FO>   # -r <n>: 51 if SIB1 carrierBandwidth=51 (20 MHz)
```
403: width unknown until its first run; run once at -r 217, then `retune_restart.sh run.log` gives the exact triple. Expect `ACQ carrier CONFIRMED from SIB1` with N_RB == SIB1 width.
At 162 the CPU load drops (FFT 2048, 61.44 MS/s): ring/GPU buffers are sized from N_RB at launch, nothing else changes.

## Not done
No unit test for DECIM (replay-only tool, verified by the replays above); filter group delay (63 input samples) not compensated; no OTA.
