/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Host side of the batched polar SC decode: builds the op list from the CPU decoder's own
 * parameters (same recursion as build_decoder_tree()/generic_polar_decoder_recursive()), and
 * finishes each decoded u vector with the CPU decoder's own info extraction + CRC so that the
 * only thing that has to be bit-exact on the GPU is u.
 */
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include "nr_polar_sc_cuda_int.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"

uint32_t crc24c(unsigned char *inptr, int bitlen);

typedef struct {
  int used, N, K, E, payloadBits, crcParityBits;
  uint16_t dci_length;
  uint8_t al;
  uint8_t info[NPC_MAX_N];
  uint16_t il[NPC_MAX_N];
} npc_host_params_t;

static npc_host_params_t g_hp[NPC_MAX_PARAMS];
static npc_dev_params_t g_dev_copy[NPC_MAX_PARAMS]; /* host copy of the op list, for npc_ref_decode */
static int g_np = 0;
static npc_timing_t g_t;
static const uint8_t *g_last_u;
static int g_nstride = NPC_MAX_N;
static uint8_t g_ref_u[NPC_MAX_N];
static int16_t *g_ref_alpha;
static int16_t g_ref_trace[4 * NPC_MAX_OPS];
const int16_t *npc_ref_last_trace(void);
const int16_t *npc_ref_last_trace(void) { return g_ref_trace; }
int npc_ref_op(int pid, int k, int *code, int *level, int *fli);
int npc_ref_op(int pid, int k, int *code, int *level, int *fli)
{ const npc_op_t o = g_dev_copy[pid].ops[k]; *code = o.code; *level = o.level; *fli = o.fli; return g_dev_copy[pid].nops; }
const int16_t *npc_ref_last_alpha(void);
const int16_t *npc_ref_last_alpha(void) { return g_ref_alpha; }

static bool all_frozen(const uint8_t *info, int fli, int sz)
{
  for (int i = 0; i < sz; i++)
    if (info[fli + i]) return false;
  return true;
}
static void emit(npc_dev_params_t *p, const uint8_t *info, int level, int fli)
{
  const int sz = 1 << (level - 1);
  const uint8_t lf = all_frozen(info, fli, sz), rf = all_frozen(info, fli + sz, sz);
  const bool lleaf = (level - 1 == 0) || lf, rleaf = (level - 1 == 0) || rf;
  p->ops[p->nops++] = (npc_op_t){NPC_OP_F, (uint8_t)level, lf, rf, (uint16_t)fli};
  if (!lleaf) emit(p, info, level - 1, fli);
  p->ops[p->nops++] = (npc_op_t){NPC_OP_G, (uint8_t)level, lf, rf, (uint16_t)fli};
  if (!rleaf) emit(p, info, level - 1, fli + sz);
  p->ops[p->nops++] = (npc_op_t){NPC_OP_B, (uint8_t)level, lf, rf, (uint16_t)fli};
}

int npc_register(uint16_t dci_length, uint8_t al)
{
  if (!dci_length || dci_length > NR_DCI_MAX_PAYLOAD || dci_length + 24 > al * 108
      || (al != 1 && al != 2 && al != 4 && al != 8 && al != 16)) return -1;
  for (int i = 0; i < g_np; i++)
    if (g_hp[i].used && g_hp[i].dci_length == dci_length && g_hp[i].al == al) return i;
  if (g_np >= NPC_MAX_PARAMS) return -1;

  t_nrPolar_params *pp = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, dci_length, al);
  npc_dev_params_t *d = calloc(1, sizeof(*d));
  d->N = pp->N; d->n = pp->n; d->E = pp->encoderLength; d->K = pp->K;
  if (d->E >= d->N) d->rm_mode = 0;
  else d->rm_mode = ((d->K / (double)d->E) <= (7.0 / 16)) ? 1 : 2;
  memcpy(d->rmp, pp->rate_matching_pattern, sizeof(uint16_t) * d->E);
  d->nops = 0;
  emit(d, pp->information_bit_pattern, d->n, 0);

  npc_host_params_t *h = &g_hp[g_np];
  h->used = 1; h->N = pp->N; h->K = pp->K; h->E = pp->encoderLength;
  h->payloadBits = pp->payloadBits; h->crcParityBits = pp->crcParityBits;
  h->dci_length = dci_length; h->al = al;
  memcpy(h->info, pp->information_bit_pattern, pp->N);
  memcpy(h->il, pp->interleaving_pattern, sizeof(uint16_t) * pp->K);
  polarReturn(pp);

  const int ok = npc_gpu_upload_params(g_np, d) == 0;
  { int npc_gpu_verify_params(int, const void *, int);
    const int bad = npc_gpu_verify_params(g_np, d, (int)sizeof(*d));
    if (bad >= 0) printf("NPC UPLOAD MISMATCH slot=%d first differing byte=%d (sizeof=%d, offsetof(ops)=%d)\n",
                         g_np, bad, (int)sizeof(*d), (int)offsetof(npc_dev_params_t, ops)); }
  g_dev_copy[g_np] = *d;
  free(d);
  return ok ? g_np++ : -1;
}

/* The tail of polar_decoder_int16(), same operations in the same order (ones_flag = 1, n_pc = 0). */
static void finish(const npc_host_params_t *h, const uint8_t *u, uint32_t *crc_out, nr_dci_bits_t *payload)
{
  uint64_t B[4] = {0};
  int k = 0;
  for (int n = 0; n < h->N; n++) {
    if (h->info[n] == 1) {
      const int targetBit = h->K - 1 - h->il[k];
      B[targetBit >> 6] |= (uint64_t)u[n] << (targetBit & 63);
      k++;
    }
  }
  const int len = h->payloadBits, crclen = h->crcParityBits;
  const uint64_t rxcrc = B[0] & ((1 << crclen) - 1);
  const uint8_t offset = 3;
  uint32_t crc = 0;
  *payload = (nr_dci_bits_t){{0}};
  uint64_t Ar = 0;
  if (len <= 32) {
    Ar = (uint32_t)(B[0] >> crclen);
    uint8_t A32_flip[4 + 3] = {0xff, 0xff, 0xff};
    const uint32_t Aprime = (uint32_t)(Ar << (32 - len));
    A32_flip[3] = ((const uint8_t *)&Aprime)[3]; A32_flip[4] = ((const uint8_t *)&Aprime)[2];
    A32_flip[5] = ((const uint8_t *)&Aprime)[1]; A32_flip[6] = ((const uint8_t *)&Aprime)[0];
    crc = (crc24c(A32_flip, 8 * offset + len) >> 8) & 0xffffff;
  } else if (len <= 64) {
    Ar = (B[0] >> crclen) | (B[1] << (64 - crclen));
    uint8_t A64_flip[8 + 3] = {0xff, 0xff, 0xff};
    const uint64_t Aprime = (uint64_t)(Ar << (64 - len));
    for (int i = 0; i < 8; i++) A64_flip[3 + i] = ((const uint8_t *)&Aprime)[7 - i];
    crc = (crc24c(A64_flip, 8 * offset + len) >> 8) & 0xffffff;
  }
  if (len > 64) {
    uint8_t packed[3 + 18] = {0xff, 0xff, 0xff};
    for (int w = 0; w < (len + 63) / 64; ++w)
      payload->w[w] = (B[w] >> crclen) | (B[w + 1] << (64 - crclen));
    for (int i = 0; i < len; ++i) {
      int bit = len - 1 - i;
      packed[3 + i / 8] |= ((payload->w[bit / 64] >> (bit % 64)) & 1) << (7 - i % 8);
    }
    crc = (crc24c(packed, 24 + len) >> 8) & 0xffffff;
  } else {
    payload->w[0] = Ar;
  }
  *crc_out = crc ^ (uint32_t)rxcrc;
}

static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }

int npc_decode_batch(const npc_item_t *items, int n, uint32_t *crc, nr_dci_bits_t *payload)
{
  static int cap = 0;
  static int *h_pid = NULL; static int16_t *h_llr = NULL; static uint8_t *h_u = NULL;
  if (n > cap) {
    free(h_pid); npc_gpu_host_free(h_llr); npc_gpu_host_free(h_u);
    cap = n;
    h_pid = malloc(sizeof(int) * cap);
    h_llr = npc_gpu_host_alloc(sizeof(int16_t) * NPC_MAX_E * cap);
    h_u = npc_gpu_host_alloc((size_t)NPC_MAX_N * cap);
  }
  /* copy only what the batch's widest item needs, not the compile-time max */
  int estride = 0, nstride = 0;
  for (int i = 0; i < n; i++) {
    const npc_host_params_t *h = &g_hp[items[i].pid];
    if (h->E > estride) estride = h->E;
    if (h->N > nstride) nstride = h->N;
  }
  for (int i = 0; i < n; i++) {
    h_pid[i] = items[i].pid;
    memcpy(h_llr + (size_t)i * estride, items[i].llr, sizeof(int16_t) * g_hp[items[i].pid].E);
  }
  double h2d, kern, d2h;
  const int rc = npc_gpu_decode(h_pid, h_llr, NULL, 0, n, estride, h_u, nstride, &h2d, &kern, &d2h);
  if (rc) return rc;
  g_last_u = h_u; g_nstride = nstride;
  const double t0 = now_us();
  for (int i = 0; i < n; i++)
    finish(&g_hp[items[i].pid], h_u + (size_t)i * nstride, &crc[i], &payload[i]);
  g_t = (npc_timing_t){h2d, kern, d2h, now_us() - t0};
  return 0;
}

/* Same as npc_decode_batch, but items share LLR vectors: item i decodes vec + vidx[i]*vstride with
 * params pid[i]. Only the n_vec distinct vectors are staged and uploaded -- a dci_length sweep
 * decodes each candidate at ~34 lengths, so this removes ~97 % of the copying. Each vector is
 * staged at the widest E any of its items needs (items sharing a vector share its AL, so all equal). */
int npc_decode_batch_vec(const int16_t *vec, int vstride, int n_vec, const int *vidx, const int *pid, int n,
                         uint32_t *crc, nr_dci_bits_t *payload)
{
  static int cap = 0, vcap = 0;
  static int16_t *h_llr = NULL; static uint8_t *h_u = NULL;
  if (n > cap) {
    npc_gpu_host_free(h_u);
    cap = n;
    h_u = npc_gpu_host_alloc((size_t)NPC_MAX_N * cap);
  }
  if (n_vec > vcap) {
    npc_gpu_host_free(h_llr);
    vcap = n_vec;
    h_llr = npc_gpu_host_alloc(sizeof(int16_t) * NPC_MAX_E * vcap);
  }
  if (h_u == NULL || h_llr == NULL) return -1;
  int estride = 0, nstride = 0;
  for (int i = 0; i < n; i++) {
    const npc_host_params_t *h = &g_hp[pid[i]];
    if (h->E > estride) estride = h->E;
    if (h->N > nstride) nstride = h->N;
  }
  if (estride > vstride) return -1;
  for (int v = 0; v < n_vec; v++)
    memcpy(h_llr + (size_t)v * estride, vec + (size_t)v * vstride, sizeof(int16_t) * estride);
  double h2d, kern, d2h;
  const int rc = npc_gpu_decode(pid, h_llr, vidx, n_vec, n, estride, h_u, nstride, &h2d, &kern, &d2h);
  if (rc) return rc;
  g_last_u = h_u; g_nstride = nstride;
  const double t0 = now_us();
  for (int i = 0; i < n; i++)
    finish(&g_hp[pid[i]], h_u + (size_t)i * nstride, &crc[i], &payload[i]);
  g_t = (npc_timing_t){h2d, kern, d2h, now_us() - t0};
  return 0;
}

npc_timing_t npc_last_timing(void) { return g_t; }

/* debug helpers for the test: last batch's u vector of item i, and the pattern tables */
const uint8_t *npc_dbg_last_u(int i);
const uint8_t *npc_dbg_last_u(int i) { return g_last_u + (size_t)i * g_nstride; }
int npc_dbg_params(int pid, int *N, int *K, const uint8_t **info, const uint16_t **il)
{
  *N = g_hp[pid].N; *K = g_hp[pid].K; *info = g_hp[pid].info; *il = g_hp[pid].il; return 0;
}

/* Reference walk of the SAME op list with the SAME formulas, in plain sequential C. If this
 * agrees with the GPU but not with polar_decoder_int16(), the formulas are wrong; if it agrees
 * with polar_decoder_int16() but not the GPU, the kernel has a race or a memory bug. */
static int16_t r_neg(int16_t x) { return x == -32768 ? x : (int16_t)-x; }
static int16_t r_abs(int16_t x) { return x < 0 ? r_neg(x) : x; }
static int16_t r_sign(int16_t x, int16_t s) { return s < 0 ? r_neg(x) : (s == 0 ? (int16_t)0 : x); }
static int16_t r_subs(int16_t a, int16_t b) { const int r = (int)a - b; return r > 32767 ? 32767 : (r < -32768 ? -32768 : (int16_t)r); }
static int16_t r_adds(int16_t a, int16_t b, int sz) { const int r = (int)a + b; const int lo = (sz & 7) ? -32767 : -32768; return r > 32767 ? 32767 : (r < lo ? (int16_t)lo : (int16_t)r); }
static int16_t r_f(int16_t a, int16_t b, int sz) { const int16_t aa = r_abs(a), ab = r_abs(b); const int16_t m = aa < ab ? aa : ab; if (sz & 3) return ((a < 0) == (b < 0)) ? m : r_neg(m); return r_sign(m, r_sign(a, b)); }

int npc_ref_decode(const npc_item_t *items, int n, uint32_t *crc, nr_dci_bits_t *payload);
int npc_ref_decode(const npc_item_t *items, int n, uint32_t *crc, nr_dci_bits_t *payload)
{
  static int16_t alpha[10 * NPC_MAX_N];
  g_ref_alpha = alpha;
  static int8_t beta[10 * NPC_MAX_N];
  static uint8_t u[NPC_MAX_N];
  for (int it = 0; it < n; it++) {
    const npc_dev_params_t *p = &g_dev_copy[items[it].pid];
    const int N = p->N;
    const int16_t *in = items[it].llr;
    int16_t *root = alpha + p->n * N;
    const int16_t fill = (p->rm_mode == 2) ? 32767 : 0;
    for (int i = 0; i < N; i++) root[i] = fill;
    for (int i = 0; i < (p->n + 1) * N; i++) beta[i] = -1;
    memset(u, 0, N);
    if (p->rm_mode == 0)
      for (int i = 0; i < p->E; i++) root[p->rmp[i]] = (int16_t)(root[p->rmp[i]] + in[i]);
    else
      for (int i = 0; i < p->E; i++) root[p->rmp[i]] = in[i];
    for (int k = 0; k < p->nops; k++) {
      const npc_op_t op = p->ops[k];
      const int L = op.level, fli = op.fli, sz = 1 << (L - 1);
      const int16_t *av = alpha + L * N + fli;
      int16_t *ac = alpha + (L - 1) * N + fli;
      int8_t *bc = beta + (L - 1) * N + fli;
      int8_t *bv = beta + L * N + fli;
      if (op.code == NPC_OP_F) {
        if (!op.lfrozen) {
          for (int i = 0; i < sz; i++) ac[i] = r_f(av[i], av[i + sz], sz);
          if (sz == 1) { const int b = ac[0] <= 0; u[fli] = b; bc[0] = b ? 1 : -1; }
        }
      } else if (op.code == NPC_OP_G) {
        if (!op.rfrozen) {
          if (op.lfrozen) for (int i = 0; i < sz; i++) ac[sz + i] = r_adds(av[sz + i], av[i], sz);
          else for (int i = 0; i < sz; i++) ac[sz + i] = r_subs(av[sz + i], r_sign(av[i], (int16_t)bc[i]));
          if (sz == 1) { const int b = ac[1] <= 0; u[fli + 1] = b; bc[1] = b ? 1 : -1; }
        }
      } else {
        for (int i = 0; i < sz; i++) { const int8_t bl = bc[i], br = bc[sz + i]; bv[i] = (bl == br) ? -1 : 1; bv[sz + i] = br; }
      }
      { int16_t t0 = 0, t1 = 0;
        if (op.code == NPC_OP_F) { if (!op.lfrozen) { t0 = ac[0]; t1 = ac[sz - 1]; } }
        else if (op.code == NPC_OP_G) { if (!op.rfrozen) { t0 = ac[sz]; t1 = ac[2 * sz - 1]; } }
        else { t0 = bv[0]; t1 = bv[2 * sz - 1]; }
        g_ref_trace[4 * k] = t0; g_ref_trace[4 * k + 1] = t1;
        g_ref_trace[4 * k + 2] = av[0]; g_ref_trace[4 * k + 3] = av[sz]; }
    }
    memcpy(g_ref_u, u, NPC_MAX_N);
    finish(&g_hp[items[it].pid], u, &crc[it], &payload[it]);
  }
  return 0;
}
const uint8_t *npc_ref_last_u(void);
const uint8_t *npc_ref_last_u(void) { return g_ref_u; }
