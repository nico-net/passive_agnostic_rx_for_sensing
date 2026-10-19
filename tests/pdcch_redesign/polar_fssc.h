// SPDX-License-Identifier: LicenseRef-CSSL-1.0
// Prototype fast-SSC DCI polar decoder (NEON, host) + code tables; shared by polar_bench.cc and gpu_bench.cu. Header-only, include once.
#pragma once
#include <arm_neon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <vector>

#define NR_DCI_WORDS 3
extern "C" {
#include "PHY/CODING/nrPolar_tools/nr_polar_defs.h"
#include "PHY/CODING/nrPolar_tools/nr_polar_dci_defs.h"
uint32_t polar_decoder_int16(int16_t *input, uint64_t *out, uint8_t ones_flag, int8_t messageType, uint16_t messageLength,
                             uint8_t aggregation_level);
void polar_encoder_fast(uint64_t *A, void *out, int32_t crcmask, uint8_t ones_flag, int8_t messageType, uint16_t messageLength,
                        uint8_t aggregation_level);
t_nrPolar_params *nr_polar_params(int8_t messageType, uint16_t messageLength, uint8_t aggregation_level);
void polarReturn(t_nrPolar_params *);
uint32_t crc24c(unsigned char *inptr, int bitlen);
void crcTableInit(void);
struct configmodule_interface_s *uniqCfg = NULL;
void *g_log = NULL;
void logRecord_mt(const char *, const char *, int, int, int, const char *, ...) {}

void exit_function(const char *, const char *, int, const char *, int) { abort(); }
}

static double now_us() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e6 + t.tv_nsec / 1e3; }
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }
static double gauss() { double u = (rnd() + 1.0) / 4294967297.0, v = (rnd() + 1.0) / 4294967297.0; return sqrt(-2 * log(u)) * cos(6.283185307 * v); }

// ---------------------------------------------------------------- prototype fast-SSC
enum { T_R0, T_R1, T_REP, T_SPC, T_SPLIT };
struct Node { uint8_t type, lvl; int16_t l, r; };
struct Code {
  int len, al, N, n, K, E;
  uint8_t punct_mode;               // 0 repetition, 1 puncture(zero fill), 2 shorten(max fill)
  std::vector<uint16_t> rmp;
  std::vector<uint8_t> info;
  std::vector<uint16_t> idx, tgt;   // k-th info position, target bit in B
  std::vector<Node> nodes; int root;
  alignas(16) int16_t alpha[10][512];
  alignas(16) int16_t dt[512];
  alignas(16) uint8_t x[512];
  alignas(16) int16_t lalpha[10][512 * 8];
  alignas(16) int16_t ldt[512 * 8];
  alignas(16) int16_t lbeta[512 * 8];
};

static int build_node(Code &c, int lvl, int s)
{
  const int sz = 1 << lvl;
  int ninfo = 0;
  for (int i = 0; i < sz; i++) ninfo += c.info[s + i];
  Node nd{T_SPLIT, (uint8_t)lvl, -1, -1};
  if (ninfo == 0) nd.type = T_R0;
  else if (ninfo == sz) nd.type = T_R1;
  else if (sz >= 4 && ninfo == 1 && c.info[s + sz - 1]) nd.type = T_REP;
  else if (sz >= 4 && ninfo == sz - 1 && !c.info[s]) nd.type = T_SPC;
  if (nd.type == T_SPLIT) {
    nd.l = (int16_t)build_node(c, lvl - 1, s);
    nd.r = (int16_t)build_node(c, lvl - 1, s + sz / 2);
  }
  c.nodes.push_back(nd);
  return (int)c.nodes.size() - 1;
}

static void code_init(Code &c, int len, int al)
{
  t_nrPolar_params *p = nr_polar_params(NR_POLAR_DCI_MESSAGE_TYPE, len, al);
  c.len = len; c.al = al; c.N = p->N; c.n = p->n; c.K = p->K; c.E = p->encoderLength;
  c.rmp.assign(p->rate_matching_pattern, p->rate_matching_pattern + c.E);
  c.info.assign(p->information_bit_pattern, p->information_bit_pattern + c.N);
  if (c.E >= c.N) c.punct_mode = 0;
  else if ((p->K / (double)c.E) <= 7.0 / 16) c.punct_mode = 1;
  else c.punct_mode = 2;
  int k = 0;
  for (int i = 0; i < c.N; i++)
    if (c.info[i]) { c.idx.push_back(i); c.tgt.push_back(c.K - 1 - p->interleaving_pattern[k]); k++; }
  polarReturn(p);
  c.root = build_node(c, c.n, 0);
}

static inline int16x8_t F8(int16x8_t a, int16x8_t b)
{
  const int16x8_t m = vminq_s16(vqabsq_s16(a), vqabsq_s16(b));
  const int16x8_t s = vshrq_n_s16(veorq_s16(a, b), 15);
  return vsubq_s16(veorq_s16(m, s), s);
}
static inline int16x8_t G8(int16x8_t a, int16x8_t b, uint16x8_t u) // u lane all-ones => b - a, else b + a
{
  return vqaddq_s16(b, vbslq_s16(u, vqnegq_s16(a), a));
}

static void run_node(Code &c, const Node *nd, const int16_t *alpha, uint8_t *out)
{
  const int sz = 1 << nd->lvl;
  switch (nd->type) {
    case T_R0: memset(out, 0, sz); return;
    case T_R1:
      if (sz >= 8)
        for (int i = 0; i < sz; i += 8) {
          const uint16x8_t m = vcleq_s16(vld1q_s16(alpha + i), vdupq_n_s16(0));
          vst1_u8(out + i, vand_u8(vmovn_u16(m), vdup_n_u8(1)));
        }
      else
        for (int i = 0; i < sz; i++) out[i] = alpha[i] <= 0;
      return;
    case T_REP: {
      int32_t s = 0;
      for (int i = 0; i < sz; i++) s += alpha[i];
      memset(out, s <= 0, sz);
      return;
    }
    case T_SPC: {
      int par = 0, mi = 0, mv = 1 << 30;
      for (int i = 0; i < sz; i++) {
        const int a = alpha[i];
        out[i] = a <= 0; par ^= out[i];
        const int ab = a < 0 ? -a : a;
        if (ab < mv) { mv = ab; mi = i; }
      }
      out[mi] ^= par;
      return;
    }
    default: break;
  }
  const int h = sz >> 1;
  int16_t *nb = c.alpha[nd->lvl - 1];
  if (h >= 8) for (int i = 0; i < h; i += 8) vst1q_s16(nb + i, F8(vld1q_s16(alpha + i), vld1q_s16(alpha + h + i)));
  else
    for (int i = 0; i < h; i++) {
      const int a = alpha[i], b = alpha[h + i];
      const int m = std::min(abs(a), abs(b));
      nb[i] = (int16_t)(((a ^ b) < 0) ? -m : m);
    }
  run_node(c, &c.nodes[nd->l], nb, out);
  if (h >= 8)
    for (int i = 0; i < h; i += 8) {
      const uint16x8_t u = vcgtq_u16(vmovl_u8(vld1_u8(out + i)), vdupq_n_u16(0));
      vst1q_s16(nb + i, G8(vld1q_s16(alpha + i), vld1q_s16(alpha + h + i), u));
    }
  else
    for (int i = 0; i < h; i++) {
      int v = out[i] ? alpha[h + i] - alpha[i] : alpha[h + i] + alpha[i];
      nb[i] = (int16_t)std::max(-32767, std::min(32767, v));
    }
  run_node(c, &c.nodes[nd->r], nb, out + h);
  if (h >= 16)
    for (int i = 0; i < h; i += 16) vst1q_u8(out + i, veorq_u8(vld1q_u8(out + i), vld1q_u8(out + h + i)));
  else
    for (int i = 0; i < h; i++) out[i] ^= out[h + i];
}

// rate dematch: E llr -> N alpha, same rules as nr_polar_rate_matching_int16
static inline void dematch(const Code &c, const int16_t *in, int16_t *o)
{
  const int N = c.N, E = c.E;
  if (c.punct_mode == 0) {
    memset(o, 0, N * 2);
    for (int i = 0; i < E; i++) o[c.rmp[i]] = (int16_t)(o[c.rmp[i]] + in[i]);
  } else {
    const int16_t fill = c.punct_mode == 1 ? 0 : INT16_MAX;
    for (int i = 0; i < N; i++) o[i] = fill;
    for (int i = 0; i < E; i++) o[c.rmp[i]] = in[i];
  }
}

static inline uint32_t crc_res(int len, uint64_t Ar, uint64_t rxcrc)
{
  uint8_t A64_flip[11] = {0xff, 0xff, 0xff};
  const uint64_t Aprime = Ar << (64 - len);
  for (int i = 0; i < 8; i++) A64_flip[3 + i] = ((const uint8_t *)&Aprime)[7 - i];
  return ((uint32_t)(crc24c(A64_flip, 24 + len) >> 8) & 0xffffff) ^ (uint32_t)rxcrc;
}

static uint32_t finish(const Code &c, const uint8_t *u, uint64_t *out)
{
  uint64_t B[4] = {0};
  for (size_t k = 0; k < c.idx.size(); k++) B[c.tgt[k] >> 6] |= (uint64_t)u[c.idx[k]] << (c.tgt[k] & 63);
  const uint64_t rxcrc = B[0] & 0xffffff;
  const uint64_t Ar = (B[0] >> 24) | (B[1] << 40);
  out[0] = Ar;
  return crc_res(c.len, Ar, rxcrc);
}

static void polar_transform_bytes(uint8_t *x, int N)
{
  for (int len = 1; len < N; len <<= 1)
    for (int s = 0; s < N; s += 2 * len) {
      if (len >= 16)
        for (int i = 0; i < len; i += 16) vst1q_u8(x + s + i, veorq_u8(vld1q_u8(x + s + i), vld1q_u8(x + s + len + i)));
      else
        for (int i = 0; i < len; i++) x[s + i] ^= x[s + len + i];
    }
}

static uint32_t fssc_decode(Code &c, const int16_t *llr, uint64_t *out)
{
  dematch(c, llr, c.dt);
  run_node(c, &c.nodes[c.root], c.dt, c.x);
  polar_transform_bytes(c.x, c.N);
  return finish(c, c.x, out);
}

// ---------------------------------------------------------------- 8-lane batch (one candidate per int16 lane)
static void run_node_l(Code &c, const Node *nd, const int16_t *alpha, int16_t *out) // [pos][8], out masks 0 / -1
{
  const int sz = 1 << nd->lvl;
  switch (nd->type) {
    case T_R0: memset(out, 0, sz * 16); return;
    case T_R1:
      for (int i = 0; i < sz; i++) vst1q_s16(out + 8 * i, vreinterpretq_s16_u16(vcleq_s16(vld1q_s16(alpha + 8 * i), vdupq_n_s16(0))));
      return;
    case T_REP: {
      int32x4_t lo = vdupq_n_s32(0), hi = vdupq_n_s32(0);
      for (int i = 0; i < sz; i++) { const int16x8_t a = vld1q_s16(alpha + 8 * i); lo = vaddw_s16(lo, vget_low_s16(a)); hi = vaddw_high_s16(hi, a); }
      const uint16x8_t m = vcombine_u16(vmovn_u32(vcleq_s32(lo, vdupq_n_s32(0))), vmovn_u32(vcleq_s32(hi, vdupq_n_s32(0))));
      for (int i = 0; i < sz; i++) vst1q_s16(out + 8 * i, vreinterpretq_s16_u16(m));
      return;
    }
    case T_SPC: {
      uint16x8_t par = vdupq_n_u16(0);
      int16x8_t mv = vdupq_n_s16(32767); uint16x8_t mi = vdupq_n_u16(0);
      for (int i = 0; i < sz; i++) {
        const int16x8_t a = vld1q_s16(alpha + 8 * i);
        const uint16x8_t b = vcleq_s16(a, vdupq_n_s16(0));
        vst1q_s16(out + 8 * i, vreinterpretq_s16_u16(b));
        par = veorq_u16(par, b);
        const int16x8_t ab = vqabsq_s16(a);
        const uint16x8_t lt = vcltq_s16(ab, mv);
        mv = vbslq_s16(lt, ab, mv); mi = vbslq_u16(lt, vdupq_n_u16((uint16_t)i), mi);
      }
      for (int i = 0; i < sz; i++) {
        const uint16x8_t hit = vceqq_u16(mi, vdupq_n_u16((uint16_t)i));
        vst1q_s16(out + 8 * i, veorq_s16(vld1q_s16(out + 8 * i), vreinterpretq_s16_u16(vandq_u16(hit, par))));
      }
      return;
    }
    default: break;
  }
  const int h = sz >> 1;
  int16_t *nb = c.lalpha[nd->lvl - 1];
  for (int i = 0; i < h; i++) vst1q_s16(nb + 8 * i, F8(vld1q_s16(alpha + 8 * i), vld1q_s16(alpha + 8 * (h + i))));
  run_node_l(c, &c.nodes[nd->l], nb, out);
  for (int i = 0; i < h; i++)
    vst1q_s16(nb + 8 * i, G8(vld1q_s16(alpha + 8 * i), vld1q_s16(alpha + 8 * (h + i)), vreinterpretq_u16_s16(vld1q_s16(out + 8 * i))));
  run_node_l(c, &c.nodes[nd->r], nb, out + 8 * h);
  for (int i = 0; i < h; i++) vst1q_s16(out + 8 * i, veorq_s16(vld1q_s16(out + 8 * i), vld1q_s16(out + 8 * (h + i))));
}

// 8x8 int16 transpose of in[j][off..off+8) -> o[pos*8 + lane]
static void transpose8(const int16_t *const in[8], int off, int16_t *o)
{
  int16x8_t r[8];
  for (int j = 0; j < 8; j++) r[j] = vld1q_s16(in[j] + off);
  int16x8x2_t t0 = vtrnq_s16(r[0], r[1]), t1 = vtrnq_s16(r[2], r[3]), t2 = vtrnq_s16(r[4], r[5]), t3 = vtrnq_s16(r[6], r[7]);
  int32x4x2_t u0 = vtrnq_s32(vreinterpretq_s32_s16(t0.val[0]), vreinterpretq_s32_s16(t1.val[0]));
  int32x4x2_t u1 = vtrnq_s32(vreinterpretq_s32_s16(t0.val[1]), vreinterpretq_s32_s16(t1.val[1]));
  int32x4x2_t u2 = vtrnq_s32(vreinterpretq_s32_s16(t2.val[0]), vreinterpretq_s32_s16(t3.val[0]));
  int32x4x2_t u3 = vtrnq_s32(vreinterpretq_s32_s16(t2.val[1]), vreinterpretq_s32_s16(t3.val[1]));
#define LO(x) vget_low_s64(vreinterpretq_s64_s32(x))
#define HI(x) vget_high_s64(vreinterpretq_s64_s32(x))
#define ST(k, a, b) vst1q_s16(o + 8 * (k), vreinterpretq_s16_s64(vcombine_s64(a, b)))
  ST(0, LO(u0.val[0]), LO(u2.val[0])); ST(1, LO(u1.val[0]), LO(u3.val[0]));
  ST(2, LO(u0.val[1]), LO(u2.val[1])); ST(3, LO(u1.val[1]), LO(u3.val[1]));
  ST(4, HI(u0.val[0]), HI(u2.val[0])); ST(5, HI(u1.val[0]), HI(u3.val[0]));
  ST(6, HI(u0.val[1]), HI(u2.val[1])); ST(7, HI(u1.val[1]), HI(u3.val[1]));
}

static void batch_decode8(Code &c, const int16_t *const llr[8], uint32_t res[8], uint64_t pay[8])
{
  const int N = c.N, E = c.E;
  int16_t *d = c.ldt;
  if (c.punct_mode == 0) memset(d, 0, N * 16);
  else { const int16x8_t f = vdupq_n_s16(c.punct_mode == 1 ? 0 : INT16_MAX); for (int i = 0; i < N; i++) vst1q_s16(d + 8 * i, f); }
  alignas(16) int16_t tb[64];
  int i = 0;
  for (; i + 8 <= E; i += 8) {
    transpose8(llr, i, tb);
    for (int j = 0; j < 8; j++) {
      int16_t *dst = d + 8 * c.rmp[i + j];
      if (c.punct_mode == 0) vst1q_s16(dst, vaddq_s16(vld1q_s16(dst), vld1q_s16(tb + 8 * j)));
      else vst1q_s16(dst, vld1q_s16(tb + 8 * j));
    }
  }
  for (; i < E; i++) {
    int16_t *dst = d + 8 * c.rmp[i];
    for (int j = 0; j < 8; j++) dst[j] = c.punct_mode == 0 ? (int16_t)(dst[j] + llr[j][i]) : llr[j][i];
  }
  run_node_l(c, &c.nodes[c.root], d, c.lbeta);
  for (int len = 1; len < N; len <<= 1)
    for (int s = 0; s < N; s += 2 * len)
      for (int k = 0; k < len; k++)
        vst1q_s16(c.lbeta + 8 * (s + k), veorq_s16(vld1q_s16(c.lbeta + 8 * (s + k)), vld1q_s16(c.lbeta + 8 * (s + len + k))));
  static const uint8_t wts[8] = {1, 2, 4, 8, 16, 32, 64, 128};
  const uint8x8_t w = vld1_u8(wts);
  uint64_t B[8][4]; memset(B, 0, sizeof(B));
  for (size_t k = 0; k < c.idx.size(); k++) {
    const uint8x8_t m = vand_u8(vmovn_u16(vld1q_u16((const uint16_t *)(c.lbeta + 8 * c.idx[k]))), vld1_u8(wts));
    (void)w;
    const unsigned bits = vaddv_u8(m);
    const int t = c.tgt[k];
    for (int j = 0; j < 8; j++) B[j][t >> 6] |= (uint64_t)((bits >> j) & 1) << (t & 63);
  }
  for (int j = 0; j < 8; j++) {
    const uint64_t Ar = (B[j][0] >> 24) | (B[j][1] << 40);
    res[j] = crc_res(c.len, Ar, B[j][0] & 0xffffff);
    pay[j] = Ar;
  }
}

