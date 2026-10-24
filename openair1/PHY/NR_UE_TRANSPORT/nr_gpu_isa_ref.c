/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* CPU references of the GPU twin self-test: see nr_gpu_isa_ref.h. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/TOOLS/tools_defs.h"
#include "nr_gpu_isa_ref.h"

typedef void (*nr_isa_dftfn_t)(uint8_t, int16_t *, int16_t *, unsigned char);
static nr_isa_dftfn_t g_dft, g_idft;
static char g_origin[600] = "none";
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* the size index of this build's DFT library (get_dft()/get_idft() without their LOG / assert on a miss) */
#define NR_ISA_DCASE(Sz) \
  case Sz:               \
    return DFT_##Sz;
#define NR_ISA_ICASE(Sz) \
  case Sz:               \
    return IDFT_##Sz;
static int dft_index(int n)
{
  switch (n) {
    FOREACH_DFTSZ(NR_ISA_DCASE)
    default:
      return -1;
  }
}
static int idft_index(int n)
{
  switch (n) {
    FOREACH_IDFTSZ(NR_ISA_ICASE)
    default:
      return -1;
  }
}

static void *open_dfts(void)
{
  Dl_info self;
  if (dladdr((void *)open_dfts, &self) && self.dli_fname) { /* libdfts.so next to this object (both are built into one dir) */
    char tmp[512], path[600];
    snprintf(tmp, sizeof(tmp), "%s", self.dli_fname);
    snprintf(path, sizeof(path), "%s/libdfts.so", dirname(tmp));
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
      snprintf(g_origin, sizeof(g_origin), "%s", path);
      return h;
    }
  }
  const char *names[] = {"libdfts.so", "./libdfts.so"};
  for (int i = 0; i < 2; i++) {
    void *h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (h) {
      snprintf(g_origin, sizeof(g_origin), "%s", names[i]);
      return h;
    }
  }
  return NULL;
}

static void resolve(void)
{
  /* 1. the process's own dft()/idft() (dfts_load.c), when loaded: exactly what the CPU FEP calls */
  dftfunc_t *pd = (dftfunc_t *)dlsym(RTLD_DEFAULT, "dft");
  idftfunc_t *pi = (idftfunc_t *)dlsym(RTLD_DEFAULT, "idft");
  if (pd && pi && *pd && *pi) {
    g_dft = (nr_isa_dftfn_t)*pd;
    g_idft = (nr_isa_dftfn_t)*pi;
    Dl_info di;
    snprintf(g_origin, sizeof(g_origin), "process dft() in %s", dladdr((void *)*pd, &di) && di.dli_fname ? di.dli_fname : "?");
    return;
  }
  /* 2. the library the CPU loads (load_dftslib: libdfts.so, dft_implementation / idft_implementation) */
  void *h = open_dfts();
  if (!h)
    return;
  g_dft = (nr_isa_dftfn_t)dlsym(h, "dft_implementation");
  g_idft = (nr_isa_dftfn_t)dlsym(h, "idft_implementation");
  if (!g_dft || !g_idft) {
    g_dft = g_idft = NULL;
    snprintf(g_origin, sizeof(g_origin), "none");
  }
}

int nr_gpu_isa_cpu_dft(int n, int inverse, const int16_t *in, int16_t *out)
{
  pthread_once(&g_once, resolve);
  const int idx = inverse ? idft_index(n) : dft_index(n);
  nr_isa_dftfn_t f = inverse ? g_idft : g_dft;
  if (!f || idx < 0 || n <= 0)
    return -1;
  const size_t bytes = ((size_t)n * 4 + 63) / 64 * 64;
  int16_t *a = aligned_alloc(64, bytes), *b = aligned_alloc(64, bytes);
  if (!a || !b) {
    free(a);
    free(b);
    return -1;
  }
  memcpy(a, in, (size_t)n * 4);
  memset(b, 0, bytes);
  f((uint8_t)idx, a, b, 1);
  memcpy(out, b, (size_t)n * 4);
  free(a);
  free(b);
  return 0;
}

const char *nr_gpu_isa_cpu_dft_origin(void)
{
  pthread_once(&g_once, resolve);
  return g_origin;
}

void nr_gpu_isa_cpu_rot(const int16_t *x, int16_t rot_r, int16_t rot_i, const int16_t *ts, int16_t *y, int n)
{
  const size_t bytes = ((size_t)n * 4 + 63) / 64 * 64;
  c16_t *a = aligned_alloc(64, bytes), *t = aligned_alloc(64, bytes);
  if (!a || !t) {
    free(a);
    free(t);
    memset(y, 0, (size_t)n * 4);
    return;
  }
  memcpy(a, x, (size_t)n * 4);
  memcpy(t, ts, (size_t)n * 4);
  c16_t rot2 = {.r = rot_r, .i = rot_i};
  rot2.i = -rot2.i; /* apply_nr_rotation_symbol_RX */
  rotate_cpx_vector(a, rot2, a, n, 15);
  mult_cpx_vector(a, t, a, n, 15);
  memcpy(y, a, (size_t)n * 4);
  free(a);
  free(t);
}
