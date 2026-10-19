/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include <simde/arm/neon.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_fssc.h"
#include "assertions.h"

#define MAXLEN 140
enum { T_R0, T_R1, T_SPLIT };
typedef struct { uint8_t type, lvl; int16_t l, r; } node_t;
typedef struct {
  int len, al, N, n, K, E, root, nnodes, nidx;
  uint8_t punct_mode; /* 0 repetition, 1 puncture (zero fill), 2 shorten (max fill) */
  uint8_t rep_periodic; /* repetition with rmp[i] == rmp[i % N]: sum the E/N blocks as vectors, scatter N values once */
  uint16_t *rmp, *idx, *tgt;
  node_t *nodes;
} code_t;

typedef struct {
  int16_t alpha[10][512] __attribute__((aligned(16)));
  int16_t dt[512] __attribute__((aligned(16)));
  uint8_t x[528] __attribute__((aligned(16)));
  int16_t lalpha[10][512 * 8] __attribute__((aligned(16)));
  int16_t ldt[512 * 8] __attribute__((aligned(16)));
  int16_t lbeta[512 * 8] __attribute__((aligned(16)));
} scratch_t;
static __thread scratch_t t_scr;

static _Atomic(code_t *) g_code[MAXLEN + 1][5];
static pthread_mutex_t g_code_mu = PTHREAD_MUTEX_INITIALIZER;

static int build_node(code_t *c, const uint8_t *info, int lvl, int s)
{
  const int sz = 1 << lvl;
  int ninfo = 0;
  for (int i = 0; i < sz; i++) ninfo += info[s + i];
  node_t nd = {ninfo == 0 ? T_R0 : ninfo == sz ? T_R1 : T_SPLIT, (uint8_t)lvl, -1, -1};
  if (nd.type != T_R0 && lvl > 0) { /* R1 keeps children: its tie fallback is the plain SC recursion */
    nd.l = (int16_t)build_node(c, info, lvl - 1, s);
    nd.r = (int16_t)build_node(c, info, lvl - 1, s + sz / 2);
  }
  c->nodes[c->nnodes] = nd;
  return c->nnodes++;
}

static const code_t *code_get(int len, int al)
{
  const int ai = al == 1 ? 0 : al == 2 ? 1 : al == 4 ? 2 : al == 8 ? 3 : al == 16 ? 4 : -1;
  AssertFatal(len > 0 && len <= MAXLEN && ai >= 0, "fssc: unsupported DCI size %d / AL %d\n", len, al);
  code_t *c = atomic_load_explicit(&g_code[len][ai], memory_order_acquire);
  if (c) return c;
  pthread_mutex_lock(&g_code_mu);
  c = atomic_load_explicit(&g_code[len][ai], memory_order_relaxed);
  if (!c) {
    t_nrPolar_params *p = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, len, al);
    AssertFatal(!p->i_bil && !p->n_pc, "fssc: DCI code with input interleaver / PC bits\n");
    c = calloc(1, sizeof(*c));
    c->len = len; c->al = al; c->N = p->N; c->n = p->n; c->K = p->K; c->E = p->encoderLength;
    c->rmp = malloc(c->E * sizeof(uint16_t));
    memcpy(c->rmp, p->rate_matching_pattern, c->E * sizeof(uint16_t));
    c->punct_mode = c->E >= c->N ? 0 : ((p->K / (double)c->E) <= 7.0 / 16) ? 1 : 2;
    c->rep_periodic = c->punct_mode == 0;
    for (int i = c->N; c->rep_periodic && i < c->E; i++) c->rep_periodic = c->rmp[i] == c->rmp[i % c->N];
    c->idx = malloc(c->K * sizeof(uint16_t));
    c->tgt = malloc(c->K * sizeof(uint16_t));
    for (int i = 0, k = 0; i < c->N; i++)
      if (p->information_bit_pattern[i]) {
        c->idx[k] = (uint16_t)i;
        c->tgt[k] = (uint16_t)(c->K - 1 - p->interleaving_pattern[k]);
        k++;
        c->nidx = k;
      }
    c->nodes = malloc((2 * c->N) * sizeof(node_t));
    c->root = build_node(c, p->information_bit_pattern, c->n, 0);
    polarReturn(p);
    atomic_store_explicit(&g_code[len][ai], c, memory_order_release);
  }
  pthread_mutex_unlock(&g_code_mu);
  return c;
}

static inline simde_int16x8_t F8(simde_int16x8_t a, simde_int16x8_t b)
{
  const simde_int16x8_t m = simde_vminq_s16(simde_vqabsq_s16(a), simde_vqabsq_s16(b));
  const simde_int16x8_t s = simde_vshrq_n_s16(simde_veorq_s16(a, b), 15);
  return simde_vsubq_s16(simde_veorq_s16(m, s), s);
}
static inline simde_int16x8_t G8(simde_int16x8_t a, simde_int16x8_t b, simde_uint16x8_t u) /* u all-ones: b - a, else b + a */
{
  return simde_vqaddq_s16(b, simde_vbslq_s16(u, simde_vqnegq_s16(a), a));
}
static inline int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
/* Right shift that keeps a full tree sum inside int16: max|llr| >> s <= 63, so N=512 leaves sum to <= 32256. 0 for every production
 * LLR (clipped to +-31), so decisions there are exactly the reference's; only a large-amplitude input is scaled (the reference breaks). */
static int llr_shift(const int16_t *llr, int E)
{
  simde_int16x8_t mx = simde_vdupq_n_s16(0);
  int i = 0;
  for (; i + 8 <= E; i += 8) mx = simde_vmaxq_s16(mx, simde_vqabsq_s16(simde_vld1q_s16(llr + i)));
  int m = simde_vmaxvq_s16(mx);
  for (; i < E; i++) { const int a = abs(llr[i]); m = a > m ? a : m; }
  int sh = 0;
  while ((m >> sh) > 63) sh++;
  return sh;
}
static inline int16_t scale_llr(int v, int sh) /* symmetric rounding */
{
  if (!sh) return sat16(v);
  const int a = (abs(v) + (1 << (sh - 1))) >> sh;
  return (int16_t)(v < 0 ? -a : a);
}
static inline int16_t F1(int a, int b)
{
  const int m = abs(a) < abs(b) ? abs(a) : abs(b);
  return (int16_t)(((a ^ b) < 0) ? -m : m);
}
static inline int16_t G1(int a, int b, int u)
{
  const int v = u ? b - a : b + a;
  return (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
}

/* ---------------------------------------------------------------- single candidate; out = beta (codeword bits) of the node */
static void run1(const code_t *c, const node_t *nd, const int16_t *alpha, uint8_t *out)
{
  const int sz = 1 << nd->lvl;
  if (nd->type == T_R0) { memset(out, 0, sz); return; }
  if (nd->lvl == 0) { out[0] = alpha[0] <= 0; return; }
  if (nd->type == T_R1) {
    int zero = 0;
    if (sz >= 8) {
      simde_uint16x8_t acc = simde_vdupq_n_u16(0);
      for (int i = 0; i < sz; i += 8) acc = simde_vorrq_u16(acc, simde_vceqq_s16(simde_vld1q_s16(alpha + i), simde_vdupq_n_s16(0)));
      zero = simde_vmaxvq_u16(acc) != 0;
    } else
      for (int i = 0; i < sz; i++) zero |= alpha[i] == 0;
    if (!zero) {
      for (int i = 0; i < sz; i++) out[i] = alpha[i] < 0; /* no zeros: <0 == <=0 */
      return;
    }
  }
  const int h = sz >> 1;
  int16_t *nb = t_scr.alpha[nd->lvl - 1];
  if (h >= 8) for (int i = 0; i < h; i += 8) simde_vst1q_s16(nb + i, F8(simde_vld1q_s16(alpha + i), simde_vld1q_s16(alpha + h + i)));
  else for (int i = 0; i < h; i++) nb[i] = F1(alpha[i], alpha[h + i]);
  run1(c, &c->nodes[nd->l], nb, out);
  if (h >= 8)
    for (int i = 0; i < h; i += 8) {
      const simde_uint16x8_t u = simde_vcgtq_u16(simde_vmovl_u8(simde_vld1_u8(out + i)), simde_vdupq_n_u16(0));
      simde_vst1q_s16(nb + i, G8(simde_vld1q_s16(alpha + i), simde_vld1q_s16(alpha + h + i), u));
    }
  else for (int i = 0; i < h; i++) nb[i] = G1(alpha[i], alpha[h + i], out[i]);
  run1(c, &c->nodes[nd->r], nb, out + h);
  for (int i = 0; i < h; i++) out[i] ^= out[h + i];
}

static void transform_bytes(uint8_t *x, int N)
{
  /* strides 1,2,4 inside a 64-bit word and 8 across a word pair, then 16 and up on whole vectors */
  for (int i = 0; i < N; i += 16) {
    uint64_t w0, w1;
    memcpy(&w0, x + i, 8);
    memcpy(&w1, x + i + 8, 8);
    w0 ^= (w0 >> 8) & 0x00ff00ff00ff00ffull;  w1 ^= (w1 >> 8) & 0x00ff00ff00ff00ffull;
    w0 ^= (w0 >> 16) & 0x0000ffff0000ffffull; w1 ^= (w1 >> 16) & 0x0000ffff0000ffffull;
    w0 ^= (w0 >> 32) & 0x00000000ffffffffull; w1 ^= (w1 >> 32) & 0x00000000ffffffffull;
    w0 ^= w1;
    memcpy(x + i, &w0, 8);
    memcpy(x + i + 8, &w1, 8);
  }
  for (int len = 16; len < N; len <<= 1)
    for (int s = 0; s < N; s += 2 * len)
      for (int i = 0; i < len; i += 16) simde_vst1q_u8(x + s + i, simde_veorq_u8(simde_vld1q_u8(x + s + i), simde_vld1q_u8(x + s + len + i)));
}

/* CRC residual and payload words from B (K = len + 24 bits, B[0] low). Same arithmetic as the tail of polar_decoder_int16(ones_flag=1). */
static uint32_t finish_B(int len, const uint64_t B[4], uint64_t *out)
{
  const uint64_t rxcrc = B[0] & 0xffffff;
  uint32_t crc;
  out[0] = out[1] = out[2] = 0;
  if (len <= 64) {
    const uint64_t Ar = (B[0] >> 24) | (B[1] << 40);
    uint8_t f[11] = {0xff, 0xff, 0xff};
    const uint64_t Ap = len == 64 ? Ar : Ar << (64 - len);
    for (int i = 0; i < 8; i++) f[3 + i] = ((const uint8_t *)&Ap)[7 - i];
    crc = (crc24c(f, 24 + len) >> 8) & 0xffffff;
    out[0] = len == 64 ? Ar : (Ar & ((1ULL << len) - 1));
  } else {
    uint8_t packed[3 + 18] = {0};
    memset(packed, 0xff, 3);
    for (int w = 0; w < (len + 63) / 64; ++w) out[w] = (B[w] >> 24) | (B[w + 1] << 40);
    for (int i = 0; i < len; ++i) {
      const int bit = len - 1 - i;
      packed[3 + i / 8] |= ((out[bit / 64] >> (bit % 64)) & 1) << (7 - i % 8);
    }
    crc = (crc24c(packed, 24 + len) >> 8) & 0xffffff;
  }
  return crc ^ (uint32_t)rxcrc;
}

uint32_t nr_polar_dci_decode_fssc(const int16_t *llr, uint64_t *out, uint16_t len, uint8_t al)
{
  const code_t *c = code_get(len, al);
  scratch_t *s = &t_scr;
  const int N = c->N, E = c->E;
  const int sh = llr_shift(llr, E);
  if (c->punct_mode == 0) {
    memset(s->dt, 0, N * 2);
    if (sh == 0 && c->rep_periodic) {
      int16_t acc[512] __attribute__((aligned(16)));
      memcpy(acc, llr, N * 2);
      for (int j = N; j + N <= E; j += N)
        for (int k = 0; k < N; k += 8)
          simde_vst1q_s16(acc + k, simde_vqaddq_s16(simde_vld1q_s16(acc + k), simde_vld1q_s16(llr + j + k)));
      const int base = (E / N) * N;
      for (int k = 0; base + k < E; k++) acc[k] = sat16(acc[k] + llr[base + k]);
      for (int k = 0; k < N; k++) s->dt[c->rmp[k]] = acc[k];
    } else if (sh == 0) /* production range: plain saturating sum */
      for (int i = 0; i < E; i++) s->dt[c->rmp[i]] = sat16(s->dt[c->rmp[i]] + llr[i]);
    else
      for (int i = 0; i < E; i++) s->dt[c->rmp[i]] = sat16(s->dt[c->rmp[i]] + scale_llr(llr[i], sh));
  } else if (sh == 0) {
    const int16_t fill = c->punct_mode == 1 ? 0 : INT16_MAX;
    for (int i = 0; i < N; i++) s->dt[i] = fill;
    for (int i = 0; i < E; i++) s->dt[c->rmp[i]] = llr[i];
  } else {
    const int16_t fill = c->punct_mode == 1 ? 0 : INT16_MAX;
    for (int i = 0; i < N; i++) s->dt[i] = fill;
    for (int i = 0; i < E; i++) s->dt[c->rmp[i]] = scale_llr(llr[i], sh);
  }
  run1(c, &c->nodes[c->root], s->dt, s->x);
  transform_bytes(s->x, N);
  uint64_t B[4] = {0};
  for (int k = 0; k < c->nidx; k++) B[c->tgt[k] >> 6] |= (uint64_t)s->x[c->idx[k]] << (c->tgt[k] & 63);
  return finish_B(len, B, out);
}

/* ---------------------------------------------------------------- 8 candidates, one per int16 lane. positions x 8 lanes; masks 0 / -1 */
static void runl(const code_t *c, const node_t *nd, const int16_t *alpha, int16_t *out)
{
  const int sz = 1 << nd->lvl;
  if (nd->type == T_R0) { memset(out, 0, sz * 16); return; }
  if (nd->type == T_R1) {
    simde_uint16x8_t acc = simde_vdupq_n_u16(0);
    for (int i = 0; i < sz; i++) acc = simde_vorrq_u16(acc, simde_vceqq_s16(simde_vld1q_s16(alpha + 8 * i), simde_vdupq_n_s16(0)));
    if (nd->lvl == 0 || simde_vmaxvq_u16(acc) == 0) { /* leaf, or no tie in any lane: hard decision == SC */
      for (int i = 0; i < sz; i++)
        simde_vst1q_s16(out + 8 * i, simde_vreinterpretq_s16_u16(simde_vcleq_s16(simde_vld1q_s16(alpha + 8 * i), simde_vdupq_n_s16(0))));
      return;
    }
  }
  const int h = sz >> 1;
  int16_t *nb = t_scr.lalpha[nd->lvl - 1];
  for (int i = 0; i < h; i++) simde_vst1q_s16(nb + 8 * i, F8(simde_vld1q_s16(alpha + 8 * i), simde_vld1q_s16(alpha + 8 * (h + i))));
  runl(c, &c->nodes[nd->l], nb, out);
  for (int i = 0; i < h; i++)
    simde_vst1q_s16(nb + 8 * i, G8(simde_vld1q_s16(alpha + 8 * i), simde_vld1q_s16(alpha + 8 * (h + i)),
                                   simde_vreinterpretq_u16_s16(simde_vld1q_s16(out + 8 * i))));
  runl(c, &c->nodes[nd->r], nb, out + 8 * h);
  for (int i = 0; i < h; i++) simde_vst1q_s16(out + 8 * i, simde_veorq_s16(simde_vld1q_s16(out + 8 * i), simde_vld1q_s16(out + 8 * (h + i))));
}

void nr_polar_dci_decode_fssc_x8(const int16_t *const llr_in[], int n, uint32_t res[], uint64_t out[], uint16_t len, uint8_t al)
{
  AssertFatal(n >= 1 && n <= NR_POLAR_FSSC_LANES, "fssc: batch of %d\n", n);
  const code_t *c = code_get(len, al);
  scratch_t *s = &t_scr;
  const int N = c->N, E = c->E;
  const int16_t *llr[8];
  for (int j = 0; j < 8; j++) llr[j] = llr_in[j < n ? j : 0]; /* spare lanes repeat lane 0: they cannot add ties */
  int sh[8];
  for (int j = 0; j < 8; j++) sh[j] = j < n ? llr_shift(llr[j], E) : sh[0];
  int16_t *d = s->ldt;
  if (c->punct_mode == 0) memset(d, 0, N * 16);
  else { const simde_int16x8_t f = simde_vdupq_n_s16(c->punct_mode == 1 ? 0 : INT16_MAX); for (int i = 0; i < N; i++) simde_vst1q_s16(d + 8 * i, f); }
  for (int i = 0; i < E; i++) {
    int16_t *dst = d + 8 * c->rmp[i];
    int16_t t[8];
    for (int j = 0; j < 8; j++) t[j] = scale_llr(llr[j][i], sh[j]);
    if (c->punct_mode == 0) simde_vst1q_s16(dst, simde_vqaddq_s16(simde_vld1q_s16(dst), simde_vld1q_s16(t)));
    else simde_vst1q_s16(dst, simde_vld1q_s16(t));
  }
  runl(c, &c->nodes[c->root], d, s->lbeta);
  for (int l2 = 1; l2 < N; l2 <<= 1)
    for (int st = 0; st < N; st += 2 * l2)
      for (int k = 0; k < l2; k++)
        simde_vst1q_s16(s->lbeta + 8 * (st + k), simde_veorq_s16(simde_vld1q_s16(s->lbeta + 8 * (st + k)), simde_vld1q_s16(s->lbeta + 8 * (st + l2 + k))));
  uint64_t B[8][4];
  memset(B, 0, sizeof(B));
  for (int k = 0; k < c->nidx; k++) {
    const int t = c->tgt[k];
    const int16_t *m = s->lbeta + 8 * c->idx[k];
    for (int j = 0; j < n; j++) B[j][t >> 6] |= (uint64_t)(m[j] & 1) << (t & 63);
  }
  for (int j = 0; j < n; j++) res[j] = finish_B(len, B[j], out + 3 * j);
}
