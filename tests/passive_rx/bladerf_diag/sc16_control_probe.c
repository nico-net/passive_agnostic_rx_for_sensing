// Control test: identical libbladeRF call pattern as sc8_probe.c, but plain SC16 Q11,
// NO oversample feature. Confirms the probe methodology itself is sound.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <libbladeRF.h>

int main(int argc, char **argv) {
  const char *serial = argc > 1 ? argv[1] : "";
  uint64_t freq = argc > 2 ? strtoull(argv[2], NULL, 10) : 3450000000ULL;
  int gain = argc > 3 ? atoi(argv[3]) : 40;
  const char *outfile = argc > 4 ? argv[4] : "/tmp/sc16_probe.bin";
  const int nsamps = 1 << 20;

  struct bladerf *dev = NULL;
  char addr[256];
  snprintf(addr, sizeof(addr), "*:serial=%s", serial);
  int status = bladerf_open(&dev, serial[0] ? addr : NULL);
  if (status != 0) { fprintf(stderr, "open failed: %s\n", bladerf_strerror(status)); return 1; }

  unsigned int actual_rate = 0;
  status = bladerf_set_sample_rate(dev, BLADERF_CHANNEL_RX(0), 30720000, &actual_rate);
  if (status != 0) { fprintf(stderr, "set_sample_rate failed: %s\n", bladerf_strerror(status)); return 1; }
  fprintf(stderr, "sample_rate actual=%u\n", actual_rate);

  status = bladerf_set_frequency(dev, BLADERF_CHANNEL_RX(0), freq);
  if (status != 0) { fprintf(stderr, "set_frequency failed: %s\n", bladerf_strerror(status)); return 1; }

  status = bladerf_set_gain_mode(dev, BLADERF_CHANNEL_RX(0), BLADERF_GAIN_MGC);
  status = bladerf_set_gain(dev, BLADERF_CHANNEL_RX(0), gain);

  status = bladerf_sync_config(dev, BLADERF_CHANNEL_RX(0), BLADERF_FORMAT_SC16_Q11_META, 32, 32768, 16, 2000);
  if (status != 0) { fprintf(stderr, "sync_config failed: %s\n", bladerf_strerror(status)); return 1; }
  status = bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), true);
  if (status != 0) { fprintf(stderr, "enable_module failed: %s\n", bladerf_strerror(status)); return 1; }

  int16_t *buf = malloc((size_t)nsamps * 2 * sizeof(int16_t));
  unsigned int total_got = 0;
  int iterations = 0;
  struct bladerf_metadata meta;
  memset(&meta, 0, sizeof(meta));
  while (total_got < (unsigned int)nsamps && iterations++ < 20000) {
    unsigned int remaining = (unsigned int)nsamps - total_got;
    meta.actual_count = 0;
    meta.flags = BLADERF_META_FLAG_RX_NOW;
    status = bladerf_sync_rx(dev, buf + (size_t)total_got * 2, remaining, &meta, 4000);
    if (status != 0) { fprintf(stderr, "sync_rx failed: %s\n", bladerf_strerror(status)); break; }
    total_got += meta.actual_count;
  }
  fprintf(stderr, "total_got=%u iterations=%d\n", total_got, iterations);

  bladerf_enable_module(dev, BLADERF_CHANNEL_RX(0), false);
  bladerf_close(dev);

  FILE *f = fopen(outfile, "wb");
  fwrite(buf, sizeof(int16_t), (size_t)total_got * 2, f);
  fclose(f);
  free(buf);
  return 0;
}
