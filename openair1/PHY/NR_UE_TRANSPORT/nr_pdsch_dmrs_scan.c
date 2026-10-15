/* TIM-PDSCH: blind PDSCH DM-RS identity scan over 0..65535 per UE (see nr_pdsch_dmrs_scan.h). */
#include "nr_pdsch_dmrs_scan.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define WWORDS 3 /* 12 bits per RB x NR_DMRS_SCAN_CHUNK RBs = 192 bits */

static uint32_t c_init_of(int symbols_per_slot, int slot, int symbol, int nid, int nscid)
{
  const uint64_t t = (((uint64_t)symbols_per_slot * slot + symbol + 1) * (((uint64_t)nid << 1) + 1)) << 17;
  return (uint32_t)((t + ((uint64_t)nid << 1) + nscid) % (1ULL << 31));
}

/* bits n0..n0+n-1 of one LFSR (taps: x1 = {3,0}, x2 = {3,2,1,0}) started from `init`, offset by Nc = 1600 */
static void lfsr_bits(uint32_t init, bool is_x1, int n0, int n, uint8_t *out)
{
  const int len = 1600 + n0 + n + 31;
  uint8_t *x = malloc((size_t)len);
  if (!x) { memset(out, 0, (size_t)n); return; }
  for (int i = 0; i < 31; i++) x[i] = (init >> i) & 1;
  for (int i = 0; i + 31 < len; i++)
    x[i + 31] = is_x1 ? (x[i + 3] ^ x[i]) : (x[i + 3] ^ x[i + 2] ^ x[i + 1] ^ x[i]);
  memcpy(out, x + 1600 + n0, (size_t)n);
  free(x);
}

void nr_dmrs_scan_gold_ref(uint32_t c_init, int n0, int n, uint8_t *out)
{
  uint8_t *a = malloc((size_t)n), *b = malloc((size_t)n);
  if (!a || !b) { free(a); free(b); memset(out, 0, (size_t)n); return; }
  lfsr_bits(1, true, n0, n, a);
  lfsr_bits(c_init, false, n0, n, b);
  for (int i = 0; i < n; i++) out[i] = a[i] ^ b[i];
  free(a); free(b);
}

static inline int bit_of(const uint64_t *w, int n) { return (int)((w[n >> 6] >> (n & 63)) & 1u); }
static inline int sgn(int x) { return x ? -1 : 1; }

/* subcarrier of pilot m (type 1, CDM group 0, CRB-0 referenced) in the OAI symbol layout */
static inline int pilot_re(int m, int N, int fco) { return (fco + 12 * (m / 6) + 2 * (m % 6)) % N; }

bool nr_dmrs_scan_chunk(const int16_t *const *rx, int nant, int N, int fco, int slot, int symbol, int symbols_per_slot,
                        int rb0, int nrb, nr_dmrs_scan_hit_t *hit)
{
  if (!rx || nant <= 0 || N <= 0 || rb0 < 0 || nrb < 2 || nrb > NR_DMRS_SCAN_CHUNK || !hit)
    return false;
  const int m_lo = 6 * rb0, np = 6 * nrb - 2, W = 12 * nrb, w0 = 2 * m_lo;
  /* pair products, independent of the identity */
  float pr[6 * NR_DMRS_SCAN_CHUNK], pi[6 * NR_DMRS_SCAN_CHUNK];
  double den = 0;
  for (int j = 0; j < np; j++) {
    const int k1 = pilot_re(m_lo + j, N, fco), k2 = pilot_re(m_lo + j + 2, N, fco);
    double r = 0, i = 0;
    for (int a = 0; a < nant; a++) {
      const double ar = rx[a][2 * k2], ai = rx[a][2 * k2 + 1], br = rx[a][2 * k1], bi = rx[a][2 * k1 + 1];
      r += ar * br + ai * bi; /* y(m+2) conj(y(m)) */
      i += ai * br - ar * bi;
    }
    pr[j] = (float)r; pi[j] = (float)i;
    den += sqrt(r * r + i * i);
  }
  if (den <= 0) return false;
  /* window bits of x1 and of the 31 unit x2 seeds */
  uint8_t tmp[12 * NR_DMRS_SCAN_CHUNK];
  uint64_t x1w[WWORDS] = {0}, B[31][WWORDS];
  memset(B, 0, sizeof(B));
  lfsr_bits(1, true, w0, W, tmp);
  for (int n = 0; n < W; n++) x1w[n >> 6] |= (uint64_t)tmp[n] << (n & 63);
  for (int b = 0; b < 31; b++) {
    lfsr_bits(1u << b, false, w0, W, tmp);
    for (int n = 0; n < W; n++) B[b][n >> 6] |= (uint64_t)tmp[n] << (n & 63);
  }
  float best = -1, best2 = -1; /* squared |S| */
  int bnid = -1, bns = 0;
  for (int nid = 0; nid < 65536; nid++) {
    float nid_best = -1;
    int nid_ns = 0;
    for (int ns = 0; ns < 2; ns++) {
      const uint32_t ci = c_init_of(symbols_per_slot, slot, symbol, nid, ns);
      uint64_t s[WWORDS] = {x1w[0], x1w[1], x1w[2]};
      for (uint32_t v = ci; v; v &= v - 1) {
        const int b = __builtin_ctz(v);
        s[0] ^= B[b][0]; s[1] ^= B[b][1]; s[2] ^= B[b][2];
      }
      float Sr = 0, Si = 0;
      for (int j = 0; j < np; j++) {
        const int a1 = bit_of(s, 2 * j), b1 = bit_of(s, 2 * j + 1), a2 = bit_of(s, 2 * j + 4), b2 = bit_of(s, 2 * j + 5);
        const int re = sgn(a1 ^ a2) + sgn(b1 ^ b2), im = sgn(a2 ^ b1) - sgn(b2 ^ a1); /* 2 conj(r(m+2)) r(m) */
        Sr += pr[j] * re - pi[j] * im;
        Si += pr[j] * im + pi[j] * re;
      }
      const float m2 = Sr * Sr + Si * Si;
      if (m2 > nid_best) { nid_best = m2; nid_ns = ns; }
    }
    if (nid_best > best) { best2 = best; best = nid_best; bnid = nid; bns = nid_ns; }
    else if (nid_best > best2) best2 = nid_best;
  }
  const float sc = (float)(sqrt(best) / 2.0 / den), sc2 = best2 > 0 ? (float)(sqrt(best2) / 2.0 / den) : 0.f;
  hit->nid = bnid; hit->nscid = bns; hit->rb0 = rb0; hit->nrb = nrb; hit->score = sc; hit->second = sc2;
  return sc >= NR_DMRS_SCAN_MIN_SCORE && sc >= NR_DMRS_SCAN_MIN_RATIO * sc2;
}

static int cmp_f(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

typedef struct { int rb0, nrb; float e; } chunk_t;
/* energy segments of the symbol (10 dB over the 25th-percentile RB), cut into chunks of <= NR_DMRS_SCAN_CHUNK RBs,
 * strongest first */
static int chunks_of(const int16_t *const *rx, int nant, int N, int fco, int n_rb, chunk_t *ch, int max)
{
  float E[275], S[275];
  for (int rb = 0; rb < n_rb; rb++) {
    double e = 0;
    for (int a = 0; a < nant; a++)
      for (int k = 0; k < 12; k++) {
        const int re = (fco + 12 * rb + k) % N;
        e += (double)rx[a][2 * re] * rx[a][2 * re] + (double)rx[a][2 * re + 1] * rx[a][2 * re + 1];
      }
    E[rb] = S[rb] = (float)e;
  }
  qsort(S, (size_t)n_rb, sizeof(float), cmp_f);
  const float floor_e = S[n_rb / 4] > 0 ? S[n_rb / 4] : 1.f, thr = floor_e * 10.f;
  int nch = 0;
  for (int rb = 0; rb < n_rb && nch < max;) {
    if (E[rb] <= thr) { rb++; continue; }
    int end = rb;
    while (end < n_rb && E[end] > thr) end++;
    for (int c = rb; c < end && nch < max; c += NR_DMRS_SCAN_CHUNK) {
      const int n = end - c < NR_DMRS_SCAN_CHUNK ? end - c : NR_DMRS_SCAN_CHUNK;
      if (n < 2) continue;
      double e = 0;
      for (int q = c; q < c + n; q++) e += E[q];
      ch[nch].rb0 = c; ch[nch].nrb = n; ch[nch].e = (float)(e / n); nch++;
    }
    rb = end;
  }
  for (int a = 1; a < nch; a++)
    for (int b = a; b > 0 && ch[b].e > ch[b - 1].e; b--) { const chunk_t t = ch[b]; ch[b] = ch[b - 1]; ch[b - 1] = t; }
  return nch;
}

/* cell-wide pool of identities found by full scans (any UE), re-tested cheaply by nr_dmrs_scan_slot_pool() */
#define POOL_MAX 32
static struct { int nid, nscid; } g_pool[POOL_MAX];
static int g_npool;
static pthread_mutex_t g_pmu = PTHREAD_MUTEX_INITIALIZER;
static void pool_add(int nid, int nscid)
{
  pthread_mutex_lock(&g_pmu);
  bool have = false;
  for (int i = 0; i < g_npool && !have; i++) have = g_pool[i].nid == nid && g_pool[i].nscid == nscid;
  if (!have && g_npool < POOL_MAX) { g_pool[g_npool].nid = nid; g_pool[g_npool].nscid = nscid; g_npool++; }
  pthread_mutex_unlock(&g_pmu);
}

int nr_dmrs_scan_slot(const int16_t *const *rx, int nant, int N, int fco, int n_rb, int slot, int symbol,
                      int symbols_per_slot, int max_chunks, nr_dmrs_scan_hit_t *hits, int max_hits)
{
  if (!rx || nant <= 0 || n_rb <= 0 || n_rb > 275 || !hits || max_hits <= 0)
    return 0;
  chunk_t ch[64];
  const int nch = chunks_of(rx, nant, N, fco, n_rb, ch, 64);
  int nh = 0;
  for (int c = 0; c < nch && c < max_chunks && nh < max_hits; c++)
    if (nr_dmrs_scan_chunk(rx, nant, N, fco, slot, symbol, symbols_per_slot, ch[c].rb0, ch[c].nrb, &hits[nh])) {
      pool_add(hits[nh].nid, hits[nh].nscid);
      nh++;
    }
  return nh;
}

int nr_dmrs_scan_slot_pool(const int16_t *const *rx, int nant, int N, int fco, int n_rb, int slot, int symbol,
                           int symbols_per_slot, int max_chunks, nr_dmrs_scan_hit_t *hits, int max_hits)
{
  if (!rx || nant <= 0 || n_rb <= 0 || n_rb > 275 || !hits || max_hits <= 0)
    return 0;
  int pid[POOL_MAX], pns[POOL_MAX], np;
  pthread_mutex_lock(&g_pmu);
  np = g_npool;
  for (int i = 0; i < np; i++) { pid[i] = g_pool[i].nid; pns[i] = g_pool[i].nscid; }
  pthread_mutex_unlock(&g_pmu);
  if (np == 0) return 0;
  chunk_t ch[64];
  const int nch = chunks_of(rx, nant, N, fco, n_rb, ch, 64);
  int nh = 0;
  for (int c = 0; c < nch && c < max_chunks && nh < max_hits; c++) {
    uint16_t crb[NR_DMRS_SCAN_CHUNK];
    for (int q = 0; q < ch[c].nrb; q++) crb[q] = (uint16_t)(ch[c].rb0 + q);
    float b1 = -1, b2 = -1;
    int bi = -1;
    for (int i = 0; i < np; i++) {
      const float v = nr_dmrs_scan_prb_score(rx, nant, N, fco, slot, symbol, symbols_per_slot, pid[i], pns[i], crb, ch[c].nrb);
      if (v > b1) { b2 = b1; b1 = v; bi = i; } else if (v > b2) b2 = v;
    }
    /* within-RB pairs only (4 per RB): a stricter floor than the full scan's, and the pool must clearly prefer one id */
    if (bi >= 0 && b1 >= NR_DMRS_SCAN_POOL_MIN_SCORE && b1 >= NR_DMRS_SCAN_MIN_RATIO * (b2 > 0 ? b2 : 0)) {
      hits[nh].nid = pid[bi]; hits[nh].nscid = pns[bi]; hits[nh].rb0 = ch[c].rb0; hits[nh].nrb = ch[c].nrb;
      hits[nh].score = b1; hits[nh].second = b2 > 0 ? b2 : 0;
      nh++;
    }
  }
  return nh;
}

void nr_dmrs_scan_prb_gold(int slot, int symbol, int symbols_per_slot, int nid, int nscid, int n, uint8_t *out)
{
  nr_dmrs_scan_gold_ref(c_init_of(symbols_per_slot, slot, symbol, nid, nscid & 1), 0, n, out);
}

float nr_dmrs_scan_prb_score_gold(const int16_t *const *rx, int nant, int N, int fco, const uint8_t *c, const uint16_t *crb,
                                  int n_crb)
{
  double Sr = 0, Si = 0, den = 0;
  for (int q = 0; q < n_crb; q++)
    for (int i = 0; i < 4; i++) {
      const int m = 6 * crb[q] + i;
      const int k1 = pilot_re(m, N, fco), k2 = pilot_re(m + 2, N, fco);
      double r = 0, im_ = 0;
      for (int a = 0; a < nant; a++) {
        const double ar = rx[a][2 * k2], ai = rx[a][2 * k2 + 1], br = rx[a][2 * k1], bi = rx[a][2 * k1 + 1];
        r += ar * br + ai * bi;
        im_ += ai * br - ar * bi;
      }
      const int a1 = c[2 * m], b1 = c[2 * m + 1], a2 = c[2 * m + 4], b2 = c[2 * m + 5];
      const int re = sgn(a1 ^ a2) + sgn(b1 ^ b2), im = sgn(a2 ^ b1) - sgn(b2 ^ a1);
      Sr += r * re - im_ * im;
      Si += r * im + im_ * re;
      den += sqrt(r * r + im_ * im_);
    }
  return den > 0 ? (float)(sqrt(Sr * Sr + Si * Si) / 2.0 / den) : 0.f;
}

float nr_dmrs_scan_prb_score(const int16_t *const *rx, int nant, int N, int fco, int slot, int symbol, int symbols_per_slot,
                             int nid, int nscid, const uint16_t *crb, int n_crb)
{
  if (!rx || nant <= 0 || !crb || n_crb <= 0 || nid < 0 || nid > 65535)
    return 0.f;
  int mx = 0;
  for (int q = 0; q < n_crb; q++) if (crb[q] > mx) mx = crb[q];
  if (mx > 274) return 0.f;
  const int nbits = 12 * (mx + 1);
  uint8_t *c = malloc((size_t)nbits);
  if (!c) return 0.f;
  nr_dmrs_scan_prb_gold(slot, symbol, symbols_per_slot, nid, nscid, nbits, c);
  const float v = nr_dmrs_scan_prb_score_gold(rx, nant, N, fco, c, crb, n_crb);
  free(c);
  return v;
}

/* ---- per-RNTI votes ---- */
#define VOTE_RNTIS 128
#define VOTE_IDS 8
#define VOTE_SEEN 16
typedef struct {
  uint16_t rnti;
  bool used;
  int slots;
  int64_t seen[VOTE_SEEN];
  int nid[VOTE_IDS], votes[VOTE_IDS];
} vote_t;
static vote_t g_v[VOTE_RNTIS];
static pthread_mutex_t g_vmu = PTHREAD_MUTEX_INITIALIZER;
/* SOLVERCOVER ISAC_PDSCH_DMRS_ID_CONTRAST=1: cell-wide base rate of every identity, over the distinct voted slots. A slot
 * votes its hits for every RNTI scheduled in it, so an identity present in most slots of the cell (a background/always-on
 * PDSCH, or a heavily scheduled co-scheduled UE) collects as many votes as a UE's own identity. Measured on TIM PCI 923
 * (OTA 2026-10-10 23:19 and replay of raw_pci923_tim_12s_tile3): one such id (65, 928 or 1280) sat in the top-2 of almost
 * every RNTI, and the busiest UE's id took the other slot, so a UE's own id often never became a hypothesis. */
#define BASE_IDS 512
#define BASE_SEEN 64
static struct { int nid, slots; } g_base[BASE_IDS];
static int g_nbase, g_base_slots;
static int64_t g_base_seen[BASE_SEEN];
static int g_base_seen_n;
static int g_contrast = -1; /* re-read by nr_dmrs_scan_reset() (tests) */
bool nr_dmrs_scan_contrast_on(void)
{
  if (g_contrast < 0) g_contrast = getenv("ISAC_PDSCH_DMRS_ID_CONTRAST") && atoi(getenv("ISAC_PDSCH_DMRS_ID_CONTRAST")) ? 1 : 0;
  return g_contrast == 1;
}
/* base_count: counts the hits of one slot cell-wide, once per distinct slot (g_vmu held) */
static void base_count(int64_t abs_slot, const nr_dmrs_scan_hit_t *hits, int n)
{
  for (int k = 0; k < BASE_SEEN && k < g_base_seen_n; k++)
    if (g_base_seen[k] == abs_slot) return;
  g_base_seen[g_base_seen_n++ % BASE_SEEN] = abs_slot;
  if (g_base_seen_n >= 2 * BASE_SEEN) g_base_seen_n -= BASE_SEEN; /* keep the modulo position, bounded counter */
  g_base_slots++;
  for (int h = 0; h < n; h++) {
    bool dup = false;
    for (int q = 0; q < h && !dup; q++) dup = hits[q].nid == hits[h].nid;
    if (dup) continue;
    int k = -1;
    for (int q = 0; q < g_nbase && k < 0; q++) if (g_base[q].nid == hits[h].nid) k = q;
    if (k < 0 && g_nbase < BASE_IDS) { k = g_nbase++; g_base[k].nid = hits[h].nid; g_base[k].slots = 0; }
    if (k >= 0) g_base[k].slots++;
  }
}
/* contrast of one identity for one RNTI: share of the RNTI's slots carrying it minus share of the OTHER voted slots carrying it */
static double id_contrast(const vote_t *v, int q)
{
  int g = 0;
  for (int k = 0; k < g_nbase; k++) if (g_base[k].nid == v->nid[q]) { g = g_base[k].slots; break; }
  const int own = v->votes[q] < v->slots ? v->votes[q] : v->slots;
  const int other_slots = g_base_slots - v->slots, other = g - own;
  const double p = v->slots > 0 ? (double)own / v->slots : 0.0;
  const double qq = other_slots > 0 && other > 0 ? (double)other / other_slots : 0.0;
  return p - qq;
}

static vote_t *vote_of(uint16_t rnti, bool create)
{
  for (int i = 0; i < VOTE_RNTIS; i++) if (g_v[i].used && g_v[i].rnti == rnti) return &g_v[i];
  if (!create) return NULL;
  for (int i = 0; i < VOTE_RNTIS; i++)
    if (!g_v[i].used) {
      memset(&g_v[i], 0, sizeof(g_v[i]));
      g_v[i].used = true; g_v[i].rnti = rnti;
      for (int k = 0; k < VOTE_SEEN; k++) g_v[i].seen[k] = -1;
      return &g_v[i];
    }
  return NULL;
}

void nr_dmrs_scan_vote(uint16_t rnti, int64_t abs_slot, const nr_dmrs_scan_hit_t *hits, int n)
{
  pthread_mutex_lock(&g_vmu);
  vote_t *v = vote_of(rnti, true);
  bool dup = false;
  for (int k = 0; v && k < VOTE_SEEN && !dup; k++) dup = v->seen[k] == abs_slot;
  if (v && !dup) {
    v->seen[v->slots % VOTE_SEEN] = abs_slot;
    v->slots++;
    if (nr_dmrs_scan_contrast_on()) base_count(abs_slot, hits, n);
    for (int h = 0; h < n; h++) {
      int k = -1;
      for (int q = 0; q < VOTE_IDS && k < 0; q++) if (v->votes[q] && v->nid[q] == hits[h].nid) k = q;
      if (k < 0) /* a free entry, else the least voted single-vote entry */
        for (int q = 0; q < VOTE_IDS && k < 0; q++) if (!v->votes[q]) k = q;
      if (k < 0)
        for (int q = 0; q < VOTE_IDS && k < 0; q++) if (v->votes[q] == 1) k = q;
      if (k < 0) continue;
      if (v->votes[k] == 0 || v->nid[k] != hits[h].nid) { v->nid[k] = hits[h].nid; v->votes[k] = 0; }
      v->votes[k]++;
    }
  }
  pthread_mutex_unlock(&g_vmu);
}

int nr_dmrs_scan_rnti_ids(uint16_t rnti, int *ids, int max)
{
  int n = 0;
  pthread_mutex_lock(&g_vmu);
  const vote_t *v = vote_of(rnti, false);
  bool taken[VOTE_IDS] = {false};
  while (v && n < max) {
    int k = -1;
    const bool ct = nr_dmrs_scan_contrast_on(); /* reorder only: an id is never dropped, the TB CRC still decides */
    for (int q = 0; q < VOTE_IDS; q++)
      if (!taken[q] && v->votes[q] >= NR_DMRS_SCAN_MIN_VOTES
          && (k < 0 || (ct ? id_contrast(v, q) > id_contrast(v, k) : v->votes[q] > v->votes[k]))) k = q;
    if (k < 0) break;
    taken[k] = true;
    ids[n++] = v->nid[k];
  }
  pthread_mutex_unlock(&g_vmu);
  return n;
}

bool nr_dmrs_scan_rnti_decided(uint16_t rnti)
{
  pthread_mutex_lock(&g_vmu);
  const vote_t *v = vote_of(rnti, false);
  int k = -1;
  const bool ct = nr_dmrs_scan_contrast_on();
  for (int q = 0; v && q < VOTE_IDS; q++)
    if (v->votes[q] >= NR_DMRS_SCAN_MIN_VOTES
        && (k < 0 || (ct ? id_contrast(v, q) > id_contrast(v, k) : v->votes[q] > v->votes[k]))) k = q;
  const bool d = k >= 0 && v->slots >= 4 && (!ct || id_contrast(v, k) >= NR_DMRS_SCAN_DECIDED_CONTRAST);
  pthread_mutex_unlock(&g_vmu);
  return d;
}

int nr_dmrs_scan_rnti_slots(uint16_t rnti)
{
  pthread_mutex_lock(&g_vmu);
  const vote_t *v = vote_of(rnti, false);
  const int s = v ? v->slots : 0;
  pthread_mutex_unlock(&g_vmu);
  return s;
}

void nr_dmrs_scan_reset(void)
{
  pthread_mutex_lock(&g_vmu);
  memset(g_v, 0, sizeof(g_v));
  g_nbase = g_base_slots = g_base_seen_n = 0;
  g_contrast = -1;
  pthread_mutex_unlock(&g_vmu);
  pthread_mutex_lock(&g_pmu);
  g_npool = 0;
  pthread_mutex_unlock(&g_pmu);
}
