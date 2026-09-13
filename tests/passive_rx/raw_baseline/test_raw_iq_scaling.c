/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "raw_iq_source.h"
#include <assert.h>
#include <emmintrin.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
  enum { N = 65536 };
  int16_t *input = malloc(N * sizeof(*input));
  int16_t *output = malloc(N * sizeof(*output));
  int16_t *expected = malloc(N * sizeof(*expected));
  assert(input && output && expected);
  for (int n = 0; n < N; ++n) input[n] = n - 32768;
  char dir[] = "/tmp/raw-iq-scaling.XXXXXX";
  assert(mkdtemp(dir));
  char path[128]; snprintf(path, sizeof(path), "%s/rx0.sc16", dir);
  int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
  assert(fd >= 0 && write(fd, input, N * sizeof(*input)) == N * sizeof(*input));
  close(fd);
  for (unsigned shift = 0; shift < 16; ++shift) {
    raw_iq_source source;
    assert(raw_iq_open(&source, dir, N / 2, 0, 1, 122880000, 3450000000, shift) == 0);
    uint64_t timestamp;
    void *buffers[] = {output};
    assert(raw_iq_read(&source, buffers, N / 2, 1, &timestamp) == N / 2);
    for (int n = 0; n < N; n += 8) {
      __m128i values = _mm_loadu_si128((const __m128i *)(input + n));
      values = _mm_sra_epi16(values, _mm_cvtsi32_si128(shift));
      _mm_storeu_si128((__m128i *)(expected + n), values);
    }
    assert(!memcmp(expected, output, N * sizeof(*output)));
    raw_iq_close(&source);
  }
  unlink(path); rmdir(dir);
  free(input); free(output); free(expected);
  puts("PASS: all 65536 int16 values x 16 shifts match production arithmetic-shift semantics");
  return 0;
}
