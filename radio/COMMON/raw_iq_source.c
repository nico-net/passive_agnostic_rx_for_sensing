/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#define _GNU_SOURCE
#include "raw_iq_source.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void raw_iq_close(raw_iq_source *s)
{
  for (unsigned i = 0; i < 8; ++i) {
    if (s->iq[i]) munmap((void *)s->iq[i], (size_t)s->samples * 4);
    s->iq[i] = NULL;
  }
}

int raw_iq_open(raw_iq_source *s, const char *root, uint64_t samples,
                uint64_t first_tick, unsigned channels, double rate,
                double center, unsigned shift)
{
  memset(s, 0, sizeof(*s));
  const uint16_t endian = 1;
  if (*(const uint8_t *)&endian != 1 || !root || !samples ||
      samples > SIZE_MAX / 4 || samples > INT64_MAX / 4 ||
      first_tick > INT64_MAX - samples || channels < 1 || channels > 8 ||
      !isfinite(rate) || rate <= 0 || !isfinite(center) || center <= 0 || shift > 15) {
    errno = EINVAL;
    return -1;
  }
  /* Offline replay must not inherit the live receiver MCL_FUTURE policy.
   * Otherwise mmap pins the entire recording and can OOM before acquisition.
   * This backend is file-only; leave the live radio locking policy untouched.
   */
  if (munlockall() != 0) return -1;
  s->samples = samples; s->first_tick = first_tick; s->channels = channels;
  s->rate = rate; s->center = s->frequency = center; s->shift = shift;
  int dir = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir < 0) return -1;
  for (unsigned i = 0; i < channels; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "rx%u.sc16", i);
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) goto fail;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)(samples * 4)) {
      close(fd); errno = EINVAL; goto fail;
    }
    void *data = mmap(NULL, (size_t)samples * 4, PROT_READ, MAP_PRIVATE, fd, 0);
    int saved = errno;
    close(fd);
    errno = saved;
    if (data == MAP_FAILED) goto fail;
    s->iq[i] = data;
  }
  close(dir);
  return 0;
fail:;
  int saved = errno;
  close(dir);
  raw_iq_close(s);
  errno = saved;
  return -1;
}

int raw_iq_tune(raw_iq_source *s, double frequency)
{
  if (!isfinite(frequency) || fabs(frequency - s->center) >= s->rate / 2) {
    errno = ERANGE;
    return -1;
  }
  s->frequency = frequency;
  return 0;
}

static int16_t quantize(raw_iq_source *s, double value)
{
  value = ldexp(value, -(int)s->shift);
  if (value > INT16_MAX) { ++s->clipped; return INT16_MAX; }
  if (value < INT16_MIN) { ++s->clipped; return INT16_MIN; }
  return (int16_t)lrint(value);
}

int raw_iq_read(raw_iq_source *s, void **buffers, int count,
                unsigned channels, uint64_t *timestamp)
{
  if (!buffers || !timestamp || count <= 0 || channels != s->channels) {
    errno = EINVAL; return -1;
  }
  for (unsigned c = 0; c < channels; ++c)
    if (!buffers[c] || !s->iq[c]) { errno = EINVAL; return -1; }
  if (!s->gap_done && s->gap_len && s->position >= s->gap_at) {
    const uint64_t before = s->position;
    s->position = (s->position + s->gap_len < s->samples) ? s->position + s->gap_len : s->samples;
    s->gap_done = 1;
    fprintf(stderr, "RAW_IQ_GAP injected at_sample=%llu skipped=%llu new_position=%llu\n",
            (unsigned long long)before, (unsigned long long)(s->position - before),
            (unsigned long long)s->position);
  }
  uint64_t left = s->samples - s->position;
  if ((uint64_t)count > left) count = (int)left;
  *timestamp = s->first_tick + s->position;
  if (!count) return 0;
  const double step = -2 * M_PI * (s->frequency - s->center) / s->rate;
  const double dr = cos(step), di = sin(step);
  for (unsigned c = 0; c < channels; ++c) {
    const int16_t *src = s->iq[c] + 2 * s->position;
    int16_t *dst = buffers[c];
    if (step == 0 && s->phase == 0) {
      if (s->shift == 0) {
        memcpy(dst, src, (size_t)count * 4);
      } else {
        /* Match the production signed arithmetic shift, including negatives. */
        const int divisor = 1 << s->shift;
        for (size_t n = 0; n < (size_t)count * 2; ++n) {
          const int value = src[n];
          dst[n] = value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
        }
      }
      continue;
    }
    double re = cos(s->phase), im = sin(s->phase);
    for (int n = 0; n < count; ++n) {
      dst[2*n] = quantize(s, src[2*n] * re - src[2*n+1] * im);
      dst[2*n+1] = quantize(s, src[2*n] * im + src[2*n+1] * re);
      double next = re * dr - im * di;
      im = re * di + im * dr;
      re = next;
      if ((n & 1023) == 1023) {
        double norm = hypot(re, im);
        re /= norm; im /= norm;
      }
    }
  }
  s->phase = remainder(s->phase + step * count, 2 * M_PI);
  s->position += count;
  return count;
}
