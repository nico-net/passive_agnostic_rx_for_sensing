// Standalone SC8/oversample probe: isolate whether the bladeRF SC8 path itself is sane,
// independent of OAI. Captures raw SC8_Q7 samples at 122.88 Msps with RX_NOW-loop
// accumulation identical to the fixed bladerf_lib.c, converts to int16 (x16, same as OAI),
// and dumps them for offline inspection (RMS / spectrum) -- no OAI code involved at all.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libbladeRF.h>

int main(int argc, char **argv) {
  const char *serial = argc > 1 ? argv[1] : "";
  uint64_t freq = argc > 2 ? strtoull(argv[2], NULL, 10) : 3450000000ULL;
  int gain = argc > 3 ? atoi(argv[3]) : 40;
  const char *outfile = argc > 4 ? argv[4] : "/tmp/sc8_probe.bin";
  const int nsamps = 1 << 20; // ~1M complex samples

  struct bladerf *dev = NULL;
  char addr[256];
  snprintf(addr, sizeof(addr), "*:serial=%s", serial);
  int status = bladerf_open(&dev, serial[0] ? addr : NULL);
  if (status != 0) { fprintf(stderr, "open failed: %s\n", bladerf_strerror(status)); return 1; }

  status = bladerf_enable_feature(dev, BLADERF_FEATURE_OVERSAMPLE, true);
  if (status != 0) { fprintf(stderr, "enable_feature failed: %s\n", bladerf_strerror(status)); return 1; }

  unsigned int req_rate = argc > 7 ? (unsigned int)strtoul(argv[7], NULL, 10) : 122880000u;
  unsigned int actual_rate = 0;
  status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), req_rate, &actual_rate);
  if (status != 0) { fprintf(stderr, "set_sample_rate failed: %s\n", bladerf_strerror(status)); return 1; }
  fprintf(stderr, "sample_rate actual=%u\n", actual_rate);

  status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), freq);
  if (status != 0) { fprintf(stderr, "set_frequency failed: %s\n", bladerf_strerror(status)); return 1; }

  unsigned int bw_req = argc > 5 ? (unsigned int)strtoul(argv[5], NULL, 10) : 56000000u;
  if (bw_req > 0) {
    unsigned int actual_bw = 0;
    status = bladerf_set_bandwidth(dev, BLADERF_CHANNEL_RX(0), bw_req, &actual_bw);
    if (status != 0) { fprintf(stderr, "set_bandwidth failed: %s\n", bladerf_strerror(status)); return 1; }
    fprintf(stderr, "bandwidth requested=%u actual=%u\n", bw_req, actual_bw);
  } else {
    fprintf(stderr, "bandwidth: left at device default (not set)\n");
  }

  status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
  if (status != 0) { fprintf(stderr, "set_gain_mode failed: %s\n", bladerf_strerror(status)); return 1; }
  status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), gain);
  if (status != 0) { fprintf(stderr, "set_gain failed: %s\n", bladerf_strerror(status)); return 1; }

  status = bladerf_set_tuning_mode(dev, BLADERF_TUNING_MODE_FPGA);
  if (status != 0) { fprintf(stderr, "set_tuning_mode failed: %s\n", bladerf_strerror(status)); return 1; }

  const unsigned int num_buffers = 32, buffer_size = 32768, num_transfers = 16, timeout_ms = 2000;
  status = bladerf_sync_config(dev, BLADERF_CHANNEL_RX(0), BLADERF_FORMAT_SC8_Q7_META,
                                num_buffers, buffer_size, num_transfers, timeout_ms);
  if (status != 0) { fprintf(stderr, "sync_config failed: %s\n", bladerf_strerror(status)); return 1; }

  // Mirror what OAI's device_init/trx_brf_start ACTUALLY does when nb_tx=1 (its default,
  // never set to 0 in any of the confs used so far): configure and enable the TX side too,
  // even though nothing is transmitted. Testing RX-only (as this probe did before) exercises
  // a configuration OAI's real runs never actually used -- if AD9361's oversample calibration
  // needs both sides configured, this is the difference.
  int do_tx_pair = argc > 6 ? atoi(argv[6]) : 1;
  if (do_tx_pair) {
    unsigned int actual_tx_rate = 0;
    status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_TX(0), req_rate, &actual_tx_rate);
    if (status != 0) fprintf(stderr, "TX set_sample_rate failed: %s\n", bladerf_strerror(status));
    status = bladerf_set_frequency(dev, BLADERF_CHANNEL_TX(0), freq);
    if (status != 0) fprintf(stderr, "TX set_frequency failed: %s\n", bladerf_strerror(status));
    const struct bladerf_range *g = NULL;
    bladerf_get_gain_range(dev, BLADERF_CHANNEL_TX(0), &g);
    if (g) bladerf_set_gain(dev, BLADERF_CHANNEL_TX(0), g->max - 10);
    status = bladerf_sync_config(dev, BLADERF_CHANNEL_TX(0), BLADERF_FORMAT_SC8_Q7_META,
                                  num_buffers, buffer_size, num_transfers, timeout_ms);
    if (status != 0) fprintf(stderr, "TX sync_config failed: %s\n", bladerf_strerror(status));
    status = bladerf_enable_module(dev, BLADERF_CHANNEL_TX(0), true);
    if (status != 0) fprintf(stderr, "TX enable_module failed: %s\n", bladerf_strerror(status));
    fprintf(stderr, "TX side configured+enabled (paired, no data ever sent)\n");
  }

  status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
  if (status != 0) { fprintf(stderr, "enable_module failed: %s\n", bladerf_strerror(status)); return 1; }

  usleep(200000); // settle: rule out a startup-transient/garbage-fill explanation

  int8_t *buf = malloc((size_t)nsamps * 2);
  unsigned int total_got = 0;
  int iterations = 0;
  struct bladerf_metadata meta;
  memset(&meta, 0, sizeof(meta));
  while (total_got < (unsigned int)nsamps && iterations++ < 20000) {
    unsigned int remaining = (unsigned int)nsamps - total_got;
    meta.actual_count = 0;
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, buf + (size_t)total_got * 2, remaining, &meta, 2 * timeout_ms);
    if (status != 0) { fprintf(stderr, "sync_rx failed: %s\n", bladerf_strerror(status)); break; }
    total_got += meta.actual_count;
  }
  fprintf(stderr, "total_got=%u iterations=%d\n", total_got, iterations);

  bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
  bladerf_close(dev);

  FILE *f = fopen(outfile, "wb");
  fwrite(buf, 1, (size_t)total_got * 2, f);
  fclose(f);
  fprintf(stderr, "wrote %u samples (SC8, 2 bytes each) to %s\n", total_got, outfile);

  // Second, independent read right after the first: if the two are byte-identical,
  // this is a frozen/replayed buffer, not live RF -- a cheap, decisive check.
  int8_t *buf2 = malloc((size_t)nsamps * 2);
  unsigned int total_got2 = 0;
  iterations = 0;
  while (total_got2 < (unsigned int)nsamps && iterations++ < 20000) {
    unsigned int remaining = (unsigned int)nsamps - total_got2;
    meta.actual_count = 0;
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, buf2 + (size_t)total_got2 * 2, remaining, &meta, 2 * timeout_ms);
    if (status != 0) break;
    total_got2 += meta.actual_count;
  }
  int identical = (total_got2 == total_got) && memcmp(buf, buf2, (size_t)total_got * 2) == 0;
  fprintf(stderr, "second read: total_got2=%u iterations=%d IDENTICAL_TO_FIRST=%d\n", total_got2, iterations, identical);

  free(buf);
  free(buf2);
  return 0;
}
