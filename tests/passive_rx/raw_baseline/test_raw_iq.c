/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "raw_iq_source.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
  char dir[] = "/tmp/raw-iq-test.XXXXXX";
  assert(mkdtemp(dir));
  int16_t input[4][32];
  for (int c = 0; c < 4; ++c) {
    for (int n = 0; n < 32; ++n) input[c][n] = c * 100 + n - 16;
    char path[128]; snprintf(path, sizeof(path), "%s/rx%d.sc16", dir, c);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && write(fd, input[c], sizeof(input[c])) == sizeof(input[c]));
    close(fd);
  }
  raw_iq_source s;
  int16_t output[4][32];
  void *b[] = {output[0], output[1], output[2], output[3]};
  uint64_t ts = 0;
  assert(raw_iq_open(&s, dir, 16, 12345, 4, 7680000, 3500000000, 0) == 0);
  assert(raw_iq_read(&s, b, 5, 3, &ts) == -1 && s.position == 0);
  assert(raw_iq_read(&s, b, 5, 4, &ts) == 5 && ts == 12345);
  for (int c = 0; c < 4; ++c) assert(!memcmp(output[c], input[c], 5 * 4));
  assert(raw_iq_read(&s, b, 16, 4, &ts) == 11 && ts == 12350);
  for (int c = 0; c < 4; ++c) assert(!memcmp(output[c], input[c] + 10, 11 * 4));
  assert(raw_iq_read(&s, b, 1, 4, &ts) == 0 && ts == 12361 && s.position == 16);
  raw_iq_close(&s);
  assert(raw_iq_open(&s, dir, 15, 0, 4, 7680000, 3500000000, 0) == -1);
  assert(raw_iq_open(&s, dir, 16, UINT64_MAX, 4, 7680000, 3500000000, 0) == -1);
  assert(raw_iq_open(&s, dir, 16, 0, 4, NAN, 3500000000, 0) == -1);
  assert(raw_iq_open(&s, dir, 16, 0, 4, 7680000, 3500000000, 0) == 0);
  assert(raw_iq_tune(&s, 3500000000 + 7680000 / 2) == -1);
  assert(raw_iq_tune(&s, 3500000000 + 7680000 / 4) == 0);
  assert(raw_iq_read(&s, b, 3, 4, &ts) == 3);
  assert(raw_iq_read(&s, b, 5, 4, &ts) == 5 && ts == 3);
  for (int c = 0; c < 4; ++c)
    for (int n = 0; n < 5; ++n) {
      double phase = -M_PI / 2 * (n + 3);
      int16_t i = input[c][2*(n+3)], q = input[c][2*(n+3)+1];
      assert(labs(output[c][2*n] - lrint(i*cos(phase)-q*sin(phase))) <= 1);
      assert(labs(output[c][2*n+1] - lrint(i*sin(phase)+q*cos(phase))) <= 1);
    }
  raw_iq_close(&s);
  for (int c = 0; c < 4; ++c) {
    char path[128]; snprintf(path, sizeof(path), "%s/rx%d.sc16", dir, c); unlink(path);
  }
  rmdir(dir);
  puts("PASS raw IQ: byte-exact four-channel reads, timestamps, EOF, invalid geometry, continuous digital tuning");
  return 0;
}
