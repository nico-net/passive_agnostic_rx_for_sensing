/*
 * bladeRF 2.0 micro: BLADERF_FEATURE_OVERSAMPLE + SC8_Q7 delivers noise, not RF content.
 *
 * Minimal, self-contained reproducer for upstream reporting to Nuand.
 *
 * What it does: captures ~1M complex samples at a REAL, strong RF signal ("cell" freq,
 * supplied by the caller -- must be a frequency with a known strong, continuous transmitter
 * nearby) and at an empty frequency 150 MHz away ("noise" freq), twice each: once via plain
 * SC16_Q11 at 30.72 Msps (no oversample), once via SC8_Q7 with BLADERF_FEATURE_OVERSAMPLE at
 * 122.88 Msps. Prints the received power ratio (cell vs noise, in dB) for both paths.
 *
 * Expected/observed on our unit (libbladeRF 2.5.0-0.2024.05, firmware 2.4.0-git-a3d5c55f,
 * FPGA tested at both v0.15.3 and v0.16.0):
 *   SC16, no oversample, 30.72 Msps: cell/noise ratio ~20+ dB  (PASS -- real signal seen)
 *   SC8,  oversample,   122.88 Msps: cell/noise ratio  <1 dB  (FAIL -- indistinguishable from noise)
 *
 * Build:   gcc -O2 -o oversample_repro oversample_repro.c -lbladeRF
 * Run:     sudo ./oversample_repro <serial> <cell_freq_hz> <gain_db>
 *          (cell_freq_hz should be a frequency with strong, continuous, known RF energy --
 *           we used a live gNB's SSB carrier; any strong, continuous local transmitter works)
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <libbladeRF.h>

#define NSAMPS (1 << 20)

static double capture_power(struct bladerf *dev, uint64_t freq, int gain, bool oversample) {
  int status;
  const unsigned int rate = oversample ? 122880000u : 30720000u;
  bladerf_format fmt = oversample ? BLADERF_FORMAT_SC8_Q7_META : BLADERF_FORMAT_SC16_Q11_META;

  // The oversample feature is a device-level (not per-process) state -- it persists across
  // bladerf_open() calls until explicitly toggled off, so a stale ON from a previous run
  // rejects a normal-range sample rate. Always set it explicitly, both ways.
  status = bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, oversample);
  if (status != 0) { fprintf(stderr, "enable_feature(%d): %s\n", oversample, bladerf_strerror(status)); exit(1); }

  unsigned int actual = 0;
  status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), rate, &actual);
  if (status != 0) { fprintf(stderr, "set_sample_rate: %s\n", bladerf_strerror(status)); exit(1); }

  status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), freq);
  if (status != 0) { fprintf(stderr, "set_frequency: %s\n", bladerf_strerror(status)); exit(1); }
  status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
  status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), gain);

  status = bladerf_sync_config(dev, BLADERF_CHANNEL_RX(0), fmt, 32, 32768, 16, 2000);
  if (status != 0) { fprintf(stderr, "sync_config: %s\n", bladerf_strerror(status)); exit(1); }
  status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
  if (status != 0) { fprintf(stderr, "enable_module: %s\n", bladerf_strerror(status)); exit(1); }

  const size_t bytes_per_sample = oversample ? 2 : 4; // SC8: I8+Q8; SC16: I16+Q16
  uint8_t *buf = malloc((size_t)NSAMPS * bytes_per_sample);
  unsigned int total_got = 0;
  struct bladerf_metadata meta;
  memset(&meta, 0, sizeof(meta));
  int iterations = 0;
  while (total_got < NSAMPS && iterations++ < 20000) {
    unsigned int remaining = NSAMPS - total_got;
    meta.actual_count = 0;
    meta.flags = BLADERF_META_FLAG_RX_NOW; // must be looped: a single call only returns
                                            // whatever is currently buffered, not the full
                                            // request -- see the OAI bladerf_lib.c history
                                            // this reproducer was extracted from.
    status = bladerf_sync_rx(dev, buf + (size_t)total_got * bytes_per_sample, remaining, &meta, 4000);
    if (status != 0) { fprintf(stderr, "sync_rx: %s\n", bladerf_strerror(status)); break; }
    total_got += meta.actual_count;
  }
  bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);

  double power = 0.0;
  for (unsigned int i = 0; i < total_got; i++) {
    double I, Q;
    if (oversample) {
      int8_t *s = (int8_t *)(buf + (size_t)i * 2);
      I = s[0] * 16.0; Q = s[1] * 16.0; // SC8_Q7 -> SC16_Q11 scale, same as the OAI driver
    } else {
      int16_t *s = (int16_t *)(buf + (size_t)i * 4);
      I = s[0]; Q = s[1];
    }
    power += I * I + Q * Q;
  }
  free(buf);
  return total_got ? power / total_got : -1.0;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "Usage: %s <serial> <cell_freq_hz> <gain_db>\n", argv[0]);
    return 1;
  }
  const char *serial = argv[1];
  uint64_t cell_freq = strtoull(argv[2], NULL, 10);
  uint64_t noise_freq = cell_freq + 150000000ULL; // 150 MHz away: empty in most bands
  int gain = atoi(argv[3]);

  struct bladerf *dev = NULL;
  char addr[256];
  snprintf(addr, sizeof(addr), "*:serial=%s", serial);
  if (bladerf_open(&dev, serial[0] ? addr : NULL) != 0) { fprintf(stderr, "open failed\n"); return 1; }

  double sc16_cell = capture_power(dev, cell_freq, gain, false);
  double sc16_noise = capture_power(dev, noise_freq, gain, false);
  double sc8_cell = capture_power(dev, cell_freq, gain, true);
  double sc8_noise = capture_power(dev, noise_freq, gain, true);

  bladerf_close(dev);

  double sc16_ratio_db = 10.0 * log10(sc16_cell / sc16_noise);
  double sc8_ratio_db = 10.0 * log10(sc8_cell / sc8_noise);

  printf("\n=== RESULT ===\n");
  printf("SC16 (no oversample, 30.72 Msps): cell/noise power ratio = %.2f dB  -> %s\n", sc16_ratio_db,
         sc16_ratio_db > 6.0 ? "PASS (real signal seen)" : "FAIL");
  printf("SC8  (oversample,   122.88 Msps): cell/noise power ratio = %.2f dB  -> %s\n", sc8_ratio_db,
         sc8_ratio_db > 6.0 ? "PASS (real signal seen)" : "FAIL (indistinguishable from noise)");
  return 0;
}
