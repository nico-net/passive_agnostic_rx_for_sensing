/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_cell.h"
#include "nr_core_pool.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(NR_CELL_MAX == NR_POOL_MAX_CELLS, "NR_CELL_MAX must equal the core pool cell cap");

__thread int nr_cell_tls_idx_ = 0;
int nr_cell_count_ = 1;
static volatile int g_fence;

static void nr_cell_fatal(const char *fmt, int v)
{
  fprintf(stderr, "nr_cell: fatal: ");
  fprintf(stderr, fmt, v);
  fputc('\n', stderr);
  exit(2);
}

void nr_cell_set_count(int n)
{
  if (n < 1 || n > NR_CELL_MAX)
    nr_cell_fatal("cell count %d out of range [1, NR_CELL_MAX]", n);
  __atomic_store_n(&nr_cell_count_, n, __ATOMIC_RELEASE);
}

void nr_cell_bind(int idx)
{
  if (idx < 0 || idx >= NR_CELL_MAX)
    nr_cell_fatal("cell index %d out of range [0, NR_CELL_MAX)", idx);
  nr_cell_tls_idx_ = idx;
}

typedef struct {
  void *(*fn)(void *);
  void *arg;
  int cell;
} nr_cell_start_t;

static void *nr_cell_trampoline(void *p)
{
  nr_cell_start_t s = *(nr_cell_start_t *)p;
  free(p);
  nr_cell_bind(s.cell);
  return s.fn(s.arg);
}

int nr_cell_thread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg, int cell)
{
  if (cell >= NR_CELL_MAX)
    nr_cell_fatal("thread cell %d out of range [0, NR_CELL_MAX)", cell);
  nr_cell_start_t *s = malloc(sizeof(*s));
  if (s == NULL)
    return ENOMEM;
  s->fn = fn;
  s->arg = arg;
  s->cell = cell < 0 ? nr_cell_idx() : cell;
  int rc = pthread_create(t, attr, nr_cell_trampoline, s);
  if (rc != 0)
    free(s);
  return rc;
}

void nr_cell_path(int cell, const char *def, char *out, size_t n)
{
  if (cell < 0 || cell >= NR_CELL_MAX)
    nr_cell_fatal("path cell %d out of range [0, NR_CELL_MAX)", cell);
  if (n == 0)
    return;
  if (def == NULL)
    def = "";
  if (cell == 0) {
    snprintf(out, n, "%s", def);
    return;
  }
  const char *slash = strrchr(def, '/');
  const char *base = slash ? slash + 1 : def;
  const char *dot = strrchr(base, '.');
  if (dot != NULL && dot != base)
    snprintf(out, n, "%.*s.cell%d%s", (int)(dot - def), def, cell, dot);
  else
    snprintf(out, n, "%s/cell%d", def, cell);
}

void nr_cell_alloc_fence(void)
{
  __atomic_store_n(&g_fence, 1, __ATOMIC_RELEASE);
}

int nr_cell_alloc_fenced(void)
{
  return __atomic_load_n(&g_fence, __ATOMIC_ACQUIRE);
}

void *nr_cell_heap_get(void **slot, size_t size, const char *name)
{
  void *cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (cur != NULL)
    return cur;
  if (nr_cell_alloc_fenced()) {
    fprintf(stderr, "nr_cell: fatal: first access of cell object '%s' after the RX fence (allocation forbidden)\n", name);
    exit(2);
  }
  void *p = calloc(1, size);
  if (p == NULL) {
    fprintf(stderr, "nr_cell: fatal: out of memory for cell object '%s' (%zu bytes)\n", name, size);
    exit(2);
  }
  void *expected = NULL;
  if (!__atomic_compare_exchange_n(slot, &expected, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    free(p);
    return expected;
  }
  return p;
}
