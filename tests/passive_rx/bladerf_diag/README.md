# bladeRF SC8/oversample isolation probes (2026-09-09)

Standalone libbladeRF programs, no OAI involved, used to isolate why
`BLADERF_FEATURE_OVERSAMPLE` + `BLADERF_FORMAT_SC8_Q7_META` at 122.88 Msps
never decodes against the live 100 MHz band78 cell, while plain SC16 at
30.72/61.44 Msps decodes cleanly.

Build: `gcc -O2 -o sc8_probe sc8_oversample_probe.c -lbladeRF` (same for sc16).

Usage: `sudo ./sc8_probe <serial> <freq_hz> <gain_db> <outfile> [bandwidth_hz|0] [tx_pair 0|1] [rate_hz]`

Result: tuned at the real cell (3450000000, strong 22.7 dB signal confirmed via
the SC16 control probe) vs 150 MHz off-band, the SC8/oversample probe shows
under 1 dB power difference -- i.e. no real RF content -- across every gain
(0-60 dB), bandwidth (default, 56 MHz), FPGA version (v0.15.3, v0.16.0), TX-pair
state, and rate (76.8, 122.88 Msps) tried. The SC16 control probe, same code
shape, shows a clean 22.7 dB signal at 30.72 Msps. Conclusion: the defect is in
BLADERF_FEATURE_OVERSAMPLE + SC8_Q7 on this specific unit/firmware, not in OAI.
