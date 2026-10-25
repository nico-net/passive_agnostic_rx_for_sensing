/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* nr_cprintf: per-cell line attribution for raw printf/fprintf sites (plan 2026-10-23-mpci-main-plan.md, B.8, Decision F5 = prefix).
 *
 *  - Process runs one cell (nr_cell_count() <= 1, the default): nr_cprintf/nr_cfprintf are exactly printf/fprintf (same bytes).
 *  - More than one cell: every line produced by a thread bound to cell k starts with "[c<k>] " (a message with embedded
 *    newlines is tagged on every line). The whole call is written under the stream lock, so tagged lines do not interleave.
 *  - Use it for WHOLE-line calls. A call that prints a partial line (no trailing newline) is tagged at its start, which
 *    is mid-line if an earlier call left the line open.
 *  - Threads without a cell bind (radio threads, anything never nr_cell_bind()-ed) read cell index 0 and are tagged [c0].
 *    GPU host callbacks / CUDA event threads are not bound either; their jobs carry the cell and must print it explicitly
 *    (the dashboard parses receiver-wide radio lines by grammar, not by prefix).
 *  - The LOG layer (log.c) uses nr_cell_tag_lines() for LOG_x() records, so LOG and nr_cprintf lines look the same.
 * Header only; needs nr_cell.c in the link (nr_cell_idx / nr_cell_count). */
#ifndef NR_CPRINTF_H
#define NR_CPRINTF_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nr_cell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In-place tagging of buf[0..len) (capacity cap, no NUL needed): "[c<cell>] " at the start and after every '\n' that is
 * not the last byte. Content that does not fit is truncated (never writes past cap). Returns the new length. */
static inline size_t nr_cell_tag_lines(char *buf, size_t len, size_t cap, int cell)
{
  char tag[24];
  const size_t tl = (size_t)snprintf(tag, sizeof(tag), "[c%d] ", cell);
  size_t lines = 0, total = 0;
  for (;;) { /* shrink the source until the tagged text fits (a few iterations at most) */
    lines = len ? 1 : 0;
    for (size_t i = 0; i + 1 < len; i++)
      lines += buf[i] == '\n';
    total = len + lines * tl;
    if (total <= cap)
      break;
    len -= (total - cap < len) ? (total - cap) : len;
  }
  if (len == 0)
    return 0;
  size_t o = total, i = len; /* backwards, in place: o >= i always, so unread bytes are never overwritten */
  while (i > 0) {
    i--;
    buf[--o] = buf[i];
    if (i == 0 || buf[i - 1] == '\n') {
      o -= tl;
      memcpy(buf + o, tag, tl);
    }
  }
  return total;
}

__attribute__((format(printf, 2, 0))) static inline int nr_cvfprintf(FILE *f, const char *fmt, va_list ap)
{
  if (nr_cell_count() <= 1)
    return vfprintf(f, fmt, ap);
  va_list ap2;
  va_copy(ap2, ap);
  const int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0)
    return n;
  size_t cap = (size_t)n + 1 + 24 * ((size_t)n / 2 + 2);
  char *b = malloc(cap);
  if (b == NULL)
    return vfprintf(f, fmt, ap);
  vsnprintf(b, (size_t)n + 1, fmt, ap);
  const size_t len = nr_cell_tag_lines(b, (size_t)n, cap, nr_cell_idx());
  flockfile(f);
  fwrite(b, 1, len, f);
  funlockfile(f);
  free(b);
  return (int)len;
}

__attribute__((format(printf, 2, 3))) static inline int nr_cfprintf_(FILE *f, const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  const int r = nr_cvfprintf(f, fmt, ap);
  va_end(ap);
  return r;
}

#define nr_cfprintf(f, ...) nr_cfprintf_((f), __VA_ARGS__)
#define nr_cprintf(...) nr_cfprintf_(stdout, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
