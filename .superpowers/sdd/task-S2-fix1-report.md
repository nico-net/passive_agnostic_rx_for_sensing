# S2 fix 1 report
Status: DONE (DGX synthetic unit tests, no OTA data).
- I1: SNR = (mean|H|^2 - noise_var)/noise_var, -100 dB when signal <= 0; 3.0 dB floor unchanged; header documents noise_var scale, one estimate per (k,l), ignored REs (M4, M5).
- M3: Inf/NaN pwr or sse now skip the PRB (isfinite check); test InfInFitSymbolInvalid.
- M2: data-aided cost note in header (S3 must measure real size). Build type not recorded in the original bench; still unrecorded (default RelWithDebInfo assumed).
- I2 tests: SnrGateAt6And1Db, NonZeroFirstPrbMaskOffset (first_prb=40, mask bits offset, bit 64 clear), InfInFitSymbolInvalid. 11/11 pass; ctest sense|isac 8/8; 24 .so; frozen check exit 0.
- Test generator bug found and fixed: signal power was normalised by 1.25 but the antenna-dependent 2-tap gain averages 1.6 (+1 dB; the old "20 dB within 1" hid it). Now normalised per antenna.
- Deviations from the request (statistics, not code): with 6 RE/PRB x 4 ant, [CORRECTED in round 2: the cause was the non-flat generator, not estimation noise] 6 dB gave 104/128 valid (p50 of survivors 6.9, within 1 dB) and 1 dB give 6/128 valid, so asserts are valid>=90 and <=12, not all/none. At 1 dB the residual alone (mean 0.3 vs 0.25) cannot be shown to reject everything; isolation is shown by the mutation instead.
- Mutation (old definition): 1 dB gives 42 valid > 12 and 6 dB p50 off by 1.55 -> SnrGateAt6And1Db FAILS; reverted, passes.
- Skipped: M1 (histogram instead of insertion sort), not needed until S3 budget is tight.

## Fix round 2 (tests only)
- Generator now normalises per (antenna, k): every RE has exactly the nominal SNR (round 1 left +1.7 dB at PRB 40 to -4.1 dB at PRB 127, so the 104/128 and 6/128 counts were deterministic fading, not estimation noise). Gaussian is Box-Muller over std::mt19937_64 (explicit uniforms), no std::normal_distribution.
- 80-seed Monte-Carlo (128 PRB, 6 RE x 4 ant): 6 dB valid mean 127.74 sd 0.44 (min 127), p50 5.99 sd 0.06 (5.84..6.15); 1 dB valid mean 0.42 sd 0.63 (max 2). Asserts: 6 dB valid >= 125 (6 sigma), p50 within 0.5 dB of 6 (8 sigma, measured max dev 0.16); 1 dB valid <= 4 (5.7 sigma). Vacuous isnan||p50>=3 assert removed; p10 assert dropped (unmeasured).
- Mutation (old SNR): 6 dB p50 off by 0.87 dB (>0.5) and 1 dB gives 23 valid (>4): test FAILS; reverted, 11/11 pass. ctest sense|isac 8/8, 24 .so, frozen exit 0.
