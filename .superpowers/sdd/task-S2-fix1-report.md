# S2 fix 1 report
Status: DONE (DGX synthetic unit tests, no OTA data).
- I1: SNR = (mean|H|^2 - noise_var)/noise_var, -100 dB when signal <= 0; 3.0 dB floor unchanged; header documents noise_var scale, one estimate per (k,l), ignored REs (M4, M5).
- M3: Inf/NaN pwr or sse now skip the PRB (isfinite check); test InfInFitSymbolInvalid.
- M2: data-aided cost note in header (S3 must measure real size). Build type not recorded in the original bench; still unrecorded (default RelWithDebInfo assumed).
- I2 tests: SnrGateAt6And1Db, NonZeroFirstPrbMaskOffset (first_prb=40, mask bits offset, bit 64 clear), InfInFitSymbolInvalid. 11/11 pass; ctest sense|isac 8/8; 24 .so; frozen check exit 0.
- Test generator bug found and fixed: signal power was normalised by 1.25 but the antenna-dependent 2-tap gain averages 1.6 (+1 dB; the old "20 dB within 1" hid it). Now normalised per antenna.
- Deviations from the request (statistics, not code): with 6 RE/PRB x 4 ant, per-PRB estimate noise makes 6 dB give 104/128 valid (p50 of survivors 6.9, within 1 dB) and 1 dB give 6/128 valid, so asserts are valid>=90 and <=12, not all/none. At 1 dB the residual alone (mean 0.3 vs 0.25) cannot be shown to reject everything; isolation is shown by the mutation instead.
- Mutation (old definition): 1 dB gives 42 valid > 12 and 6 dB p50 off by 1.55 -> SnrGateAt6And1Db FAILS; reverted, passes.
- Skipped: M1 (histogram instead of insertion sort), not needed until S3 budget is tight.
