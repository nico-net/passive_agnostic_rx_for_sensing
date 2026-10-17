/* Offline CPU brute force of the PUSCH data-scrambling ID (U4 Step 0, docs/superpowers/plans/2026-10-17-u4-ul-dataid-search.md).
 * Input: ISAC_UL_LLR_DUMP sidecars (.txt) / directories. Per grant, all n_ID in 0..1023 are tried on code block 0:
 * neutral = llr * sign(c(tried_id, rnti)); candidate llr = neutral * sign(c(n_ID, rnti)); deinterleave, rate-dematch, CPU LDPC with
 * CRC early stop (CB CRC24B when C > 1, TB CRC when C == 1). Hits are split by cell (PCI) and DCI format; an ID that passes on
 * >= 2 grants in different slots of one cell is the confirmation (CRC16 alone gives ~1.6 % spurious hits per grant-search).
 * usage: nr_ul_nid_bruteforce [--ldpc libldpc_orig.so] [--ids a:b] [--rv-all] <sidecar.txt|dir>...
 *        nr_ul_nid_bruteforce --selfcheck [--ldpc libldpc_orig.so]   (planted-ID control; exit 0 = pass) */
#include <dirent.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_segment/nr_rate_matching.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "openair1/PHY/gold.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"

configmodule_interface_t *uniqCfg = NULL;
int cpu_meas_enabled = 0; /* referenced by libldpc*.so */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "%s:%d %s() %s\n", file, line, function, s ? s : "");
  exit(1);
}

static LDPC_decoderfunc_t *g_dec;
static LDPC_encoderfunc_t *g_enc;

static void gold_sign(int16_t *l, uint32_t n, uint32_t cinit)
{
  uint32_t x1 = 0, x2 = cinit;
  uint32_t w = gold_generic(&x1, &x2, 1);
  for (uint32_t i = 0; i < n; i++) {
    if (i && (i & 31) == 0) w = gold_generic(&x1, &x2, 0);
    if ((w >> (i & 31)) & 1) l[i] = (int16_t)-l[i];
  }
}

typedef struct {
  uint32_t A, G; /* TBS bits, coded bits */
  int BG, Qm, Nl, rv;
} cb_par_t;

/* code block 0 of the grant at one n_ID; returns the iteration count (>= 0) when the CRC passed, -1 otherwise */
static int decode_cb0(const int16_t *llr, const cb_par_t *p)
{
  static int16_t e[300000], d[68 * 384 * 3 + 64], z[68 * 384 + 64];
  static int8_t l8[68 * 384 + 64];
  static uint8_t out[27008];
  unsigned C = 0, K = 0, Z = 0, F = 0;
  nr_segmentation(NULL, NULL, (unsigned)lenWithCrc(1, (int)p->A), &C, &K, &Z, &F, (uint8_t)p->BG);
  if (!C) return -1;
  const int E = nr_get_E(p->G, (uint8_t)C, (uint8_t)p->Qm, (uint8_t)p->Nl, 0);
  if (E <= 0 || E > (int)(sizeof(e) / 2)) return -1;
  nr_deinterleaving_ldpc((uint32_t)E, (uint8_t)p->Qm, e, (int16_t *)llr);
  memset(d, 0, sizeof(d));
  if (nr_rate_matching_ldpc_rx(0, (uint8_t)p->BG, (uint16_t)Z, d, e, (uint8_t)C, (uint8_t)p->rv, 1, (uint32_t)E, F, K - F - 2 * Z) == -1)
    return -1;
  const int Kc = p->BG == 2 ? 52 : 68, Kprime = (int)(K - F);
  memset(z, 0, 2 * Z * sizeof(*z));
  memset(z + Kprime, 127, F * sizeof(*z));
  memcpy(z + 2 * Z, d, (Kprime - 2 * Z) * sizeof(*z));
  memcpy(z + K, d + (K - 2 * Z), (Kc * Z - K) * sizeof(*z));
  for (int i = 0; i < Kc * (int)Z; i++) { /* saturate to 8 bit exactly like nrLDPC_coding_segment_decoder.c (packs) */
    int v = z[i];
    l8[i] = (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v);
  }
  int llrLen = 0;
  t_nrLDPC_dec_params dp = {.check_crc = check_crc};
  dp.BG = (uint8_t)p->BG;
  dp.Z = (uint16_t)Z;
  dp.numMaxIter = 20;
  dp.outMode = nrLDPC_outMode_BIT;
  dp.R = (uint8_t)nr_get_R_ldpc_decoder(p->rv, E, p->BG, (int)Z, &llrLen, 0);
  dp.crc_type = crcType((int)C, (int)p->A);
  dp.Kprime = lenWithCrc((int)C, (int)p->A);
  t_nrLDPC_time_stats ts;
  memset(&ts, 0, sizeof(ts));
  decode_abort_t ab;
  init_abort(&ab);
  set_abort(&ab, false);
  const int it = g_dec(&dp, l8, out, &ts, &ab);
  if (it >= dp.numMaxIter) return -1;
  int nz = 0;
  for (int i = 0; i < (Kprime >> 3); i++) nz |= out[i] != 0;
  return nz ? it : -1; /* the all-zero word passes any CRC by construction (the receiver's ZERO_TB), never a hit */
}

/* llr_tried: LLRs descrambled with tried_id. Fills hit_id/hit_it (cap) and returns the number of passing IDs in [lo, hi]. */
static int bruteforce(const int16_t *llr_tried, const cb_par_t *p, unsigned rnti, unsigned tried_id, int lo, int hi, int *hit_id,
                      int *hit_it, int cap)
{
  int16_t *neutral = malloc((size_t)p->G * 2 + 64), *w = malloc((size_t)p->G * 2 + 64);
  memcpy(neutral, llr_tried, (size_t)p->G * 2);
  gold_sign(neutral, p->G, (rnti << 15) + tried_id);
  int n = 0;
  for (int id = lo; id <= hi; id++) {
    memcpy(w, neutral, (size_t)p->G * 2);
    gold_sign(w, p->G, (rnti << 15) + (unsigned)id);
    const int it = decode_cb0(w, p);
    if (it >= 0 && n < cap) { hit_id[n] = id; hit_it[n] = it; n++; }
  }
  free(neutral);
  free(w);
  return n;
}

/* ---------------------------------------------------------------- selfcheck: planted ID, independent spec LFSR */
static void ref_gold(uint8_t *c, int n, uint32_t cinit) /* TS 38.211 5.2.1, bit by bit, Nc = 1600 */
{
  int N = 1600 + n + 32;
  uint8_t *x1 = calloc(N, 1), *x2 = calloc(N, 1);
  x1[0] = 1;
  for (int i = 0; i < 31; i++) x2[i] = (cinit >> i) & 1;
  for (int i = 0; i + 31 < N; i++) {
    x1[i + 31] = (x1[i + 3] + x1[i]) & 1;
    x2[i + 31] = (x2[i + 3] + x2[i + 2] + x2[i + 1] + x2[i]) & 1;
  }
  for (int i = 0; i < n; i++) c[i] = (x1[i + 1600] + x2[i + 1600]) & 1;
  free(x1);
  free(x2);
}

static double gauss(unsigned *s)
{
  double u = (rand_r(s) + 1.0) / (RAND_MAX + 2.0), v = (rand_r(s) + 1.0) / (RAND_MAX + 2.0);
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

/* encode a random TB exactly like the production chain (CRC, segmentation, LDPC, rate match, interleave); returns bits (0/1) of the
 * codeword b[G] before scrambling */
static uint8_t *encode_tb(const cb_par_t *p, unsigned seed)
{
  const uint32_t A = p->A;
  uint8_t *a = calloc(A / 8 + 8, 1);
  for (uint32_t i = 0; i < A / 8; i++) a[i] = (uint8_t)(rand_r(&seed) >> 7);
  uint32_t crc, B;
  if (A > NR_MAX_PDSCH_TBS) {
    crc = crc24a(a, A) >> 8;
    a[A >> 3] = ((uint8_t *)&crc)[2]; a[1 + (A >> 3)] = ((uint8_t *)&crc)[1]; a[2 + (A >> 3)] = ((uint8_t *)&crc)[0];
    B = A + 24;
  } else {
    crc = crc16(a, A) >> 16;
    a[A >> 3] = ((uint8_t *)&crc)[1]; a[1 + (A >> 3)] = ((uint8_t *)&crc)[0];
    B = A + 16;
  }
  unsigned C, K, Z, F;
  if (nr_segmentation(NULL, NULL, B, &C, &K, &Z, &F, (uint8_t)p->BG) < 0) return NULL;
  uint8_t **c = calloc(C, sizeof(*c));
  for (unsigned r = 0; r < C; r++) c[r] = calloc(K / 8 + 64, 1);
  const int Kb = nr_segmentation(a, c, B, &C, &K, &Z, &F, (uint8_t)p->BG);
  uint8_t *b = calloc(p->G + 64, 1);
  uint32_t off = 0;
  for (unsigned r = 0; r < C; r++) {
    const int E = nr_get_E(p->G, (uint8_t)C, (uint8_t)p->Qm, (uint8_t)p->Nl, (uint8_t)r);
    static uint8_t dout[68 * 384];
    memset(dout, 0, sizeof(dout));
    encoder_implemparams_t impp = {.Zc = Z, .Kb = Kb, .BG = (uint8_t)p->BG, .K = K, .gen_code = 0};
    unsigned char *in[1] = {c[r]};
    g_enc(in, dout, &impp);
    uint8_t *e = calloc(E + 64, 1);
    if (nr_rate_matching_ldpc(0, (uint8_t)p->BG, (uint16_t)Z, dout, e, (uint8_t)C, F, K - F - 2 * Z, (uint8_t)p->rv, (uint32_t)E) != 0)
      return NULL;
    nr_interleaving_ldpc((uint32_t)E, (uint8_t)p->Qm, e, b + off);
    off += E;
    free(e);
    free(c[r]);
  }
  free(c);
  free(a);
  return b;
}

static int selfcheck(void)
{
  const struct { uint32_t A, G; int BG; } cs[] = {{256, 1008, 2}, {544, 2016, 2}, {9000, 19000, 1}};
  const unsigned rnti = 0x585a, planted = 777, tried = 415;
  const double amp = 40, sigma = 28; /* amp/sigma = 1.43 per bit */
  int fail = 0;
  for (unsigned k = 0; k < 3; k++) {
    cb_par_t p = {.A = cs[k].A, .G = cs[k].G, .BG = cs[k].BG, .Qm = 2, .Nl = 1, .rv = 0};
    uint8_t *b = encode_tb(&p, 100 + k);
    uint8_t *sc = malloc(p.G);
    ref_gold(sc, (int)p.G, (rnti << 15) + planted);
    int16_t *rx = malloc(p.G * 2);
    unsigned s = 7 + k;
    for (uint32_t i = 0; i < p.G; i++) {
      double v = ((b[i] ^ sc[i]) ? -amp : amp) + sigma * gauss(&s);
      rx[i] = (int16_t)lrint(v);
    }
    gold_sign(rx, p.G, (rnti << 15) + tried); /* what the receiver holds: descrambled with the wrong id */
    int hid[8], hit[8];
    const int n = bruteforce(rx, &p, rnti, tried, 0, 1023, hid, hit, 8);
    /* CRC16 TBs (A <= 3824): the decoder checks the CRC every iteration, so ~3e-4 spurious passes per candidate (measured below),
     * i.e. ~0.3 per 1024-ID search; the planted ID must be in the set and the set tiny. CRC24 TBs: exactly {planted}. */
    int has = 0;
    for (int j = 0; j < n; j++) has |= hid[j] == (int)planted;
    const int ok = p.A > 3824 ? (n == 1 && has) : (has && n <= 3);
    printf("selfcheck tbs=%u G=%u BG%d planted=%u tried=%u -> hits=%d first=%d %s\n", p.A, p.G, p.BG, planted, tried, n, n ? hid[0] : -1,
           ok ? "PASS" : "FAIL");
    fail |= !ok;
    if (k != 1) { /* noise only: CRC16 spurious hits ~3e-4 per candidate (expected ~6 in 20 searches), CRC24: none */
      int spur = 0;
      for (int t = 0; t < 20; t++) {
        for (uint32_t i = 0; i < p.G; i++) rx[i] = (int16_t)lrint(sigma * gauss(&s));
        spur += bruteforce(rx, &p, rnti, tried, 0, 1023, hid, hit, 8);
      }
      const int lim = p.A > 3824 ? 0 : 20;
      printf("selfcheck noise-only tbs=%u: spurious hits %d over 20 searches (limit %d) %s\n", p.A, spur, lim, spur <= lim ? "PASS" : "FAIL");
      fail |= spur > lim;
    }
    free(b); free(sc); free(rx);
  }
  printf("selfcheck %s\n", fail ? "FAIL" : "PASS");
  return fail;
}

/* ---------------------------------------------------------------- dump files */
typedef struct {
  char sidecar[600], llr[600], fmt[8], ss[16];
  unsigned pci, rnti, tried, rv;
  uint64_t slot;
  cb_par_t p;
  int crc_ok, mcs, nrb;
  float snr;
} grant_t;

static int kv(const char *line, const char *key, char *out, size_t cap)
{
  char pat[40];
  snprintf(pat, sizeof(pat), " %s=", key);
  char *buf = malloc(strlen(line) + 2);
  sprintf(buf, " %s", line);
  const char *q = strstr(buf, pat);
  int ok = 0;
  if (q) {
    q += strlen(pat);
    size_t n = strcspn(q, " \n");
    if (n < cap) { memcpy(out, q, n); out[n] = 0; ok = 1; }
  }
  free(buf);
  return ok;
}

static int parse(const char *path, grant_t *g)
{
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  char line[3000], v[600];
  if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
  fclose(f);
  memset(g, 0, sizeof(*g));
  snprintf(g->sidecar, sizeof(g->sidecar), "%s", path);
#define NUM(k, dst, cast) do { if (!kv(line, k, v, sizeof(v))) return 0; dst = (cast)strtoull(v, NULL, 0); } while (0)
  NUM("pci", g->pci, unsigned); NUM("rnti", g->rnti, unsigned); NUM("tried_id", g->tried, unsigned); NUM("abs_slot", g->slot, uint64_t);
  NUM("tbs", g->p.A, uint32_t); NUM("G", g->p.G, uint32_t); NUM("bg", g->p.BG, int); NUM("qm", g->p.Qm, int);
  NUM("nl", g->p.Nl, int); NUM("rv", g->p.rv, int); NUM("crc_ok", g->crc_ok, int); NUM("mcs", g->mcs, int); NUM("num_rb", g->nrb, int);
  if (!kv(line, "dci_format", g->fmt, sizeof(g->fmt)) || !kv(line, "search_space", g->ss, sizeof(g->ss)) || !kv(line, "llr_file", v, sizeof(v)))
    return 0;
  if (kv(line, "snr_db", v, sizeof(v))) g->snr = (float)atof(v);
  snprintf(g->llr, sizeof(g->llr), "%s", path);
  char *sl = strrchr(g->llr, '/');
  kv(line, "llr_file", v, sizeof(v));
  snprintf(sl ? sl + 1 : g->llr, sizeof(g->llr) - (sl ? (size_t)(sl + 1 - g->llr) : 0), "%s", v);
  return g->p.A > 0 && g->p.G > 0 && g->p.BG >= 1 && g->p.BG <= 2;
}

typedef struct { unsigned pci, nid; char fmt[8]; uint64_t slot[64]; int ns, ngrant; } agg_t;
static agg_t g_agg[4096];
static int g_nagg;

static void add_hit(const grant_t *g, int nid)
{
  agg_t *a = NULL;
  for (int i = 0; i < g_nagg && !a; i++)
    if (g_agg[i].pci == g->pci && g_agg[i].nid == (unsigned)nid && !strcmp(g_agg[i].fmt, g->fmt)) a = &g_agg[i];
  if (!a) {
    if (g_nagg == 4096) return;
    a = &g_agg[g_nagg++];
    a->pci = g->pci; a->nid = (unsigned)nid; snprintf(a->fmt, sizeof(a->fmt), "%s", g->fmt);
  }
  a->ngrant++;
  for (int i = 0; i < a->ns; i++) if (a->slot[i] == g->slot) return;
  if (a->ns < 64) a->slot[a->ns++] = g->slot;
}

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int main(int argc, char **argv)
{
  const char *lib = "./libldpc_orig.so";
  int lo = 0, hi = 1023, rv_all = 0, self = 0;
  static char files[20000][600];
  int nf = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--ldpc") && i + 1 < argc) lib = argv[++i];
    else if (!strcmp(argv[i], "--ids") && i + 1 < argc) sscanf(argv[++i], "%d:%d", &lo, &hi);
    else if (!strcmp(argv[i], "--rv-all")) rv_all = 1;
    else if (!strcmp(argv[i], "--selfcheck")) self = 1;
    else {
      DIR *D = opendir(argv[i]);
      if (!D) { if (nf < 20000) snprintf(files[nf++], 600, "%s", argv[i]); continue; }
      struct dirent *e;
      while ((e = readdir(D)))
        if (strstr(e->d_name, ".txt") && nf < 20000) snprintf(files[nf++], 600, "%s/%s", argv[i], e->d_name);
      closedir(D);
    }
  }
  logInit();
  set_glog(OAILOG_DISABLE);
  crcTableInit(); /* without it every CRC computes to 0 and any zero-tailed word "passes" */
  void *h = dlopen(lib, RTLD_LAZY);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 2; }
  LDPC_initfunc_t *ini = (LDPC_initfunc_t *)dlsym(h, "LDPCinit");
  g_dec = (LDPC_decoderfunc_t *)dlsym(h, "LDPCdecoder");
  g_enc = (LDPC_encoderfunc_t *)dlsym(h, "LDPCencoder");
  if (!g_dec || !g_enc) { fprintf(stderr, "%s: no LDPCdecoder/LDPCencoder\n", lib); return 2; }
  if (ini) ini();
  if (self) return selfcheck();
  qsort(files, nf, 600, cmp_str);
  int ngr = 0;
  for (int fi = 0; fi < nf; fi++) {
    grant_t g;
    if (!parse(files[fi], &g)) { fprintf(stderr, "skip %s\n", files[fi]); continue; }
    FILE *f = fopen(g.llr, "rb");
    int16_t *llr = malloc((size_t)g.p.G * 2 + 64);
    if (!f || fread(llr, 2, g.p.G, f) != g.p.G) { fprintf(stderr, "bad llr %s\n", g.llr); if (f) fclose(f); free(llr); continue; }
    fclose(f);
    int rvs[4] = {g.p.rv, 0, 0, 0}, nrv = 1;
    if (rv_all) { nrv = 4; for (int r = 0; r < 4; r++) rvs[r] = r; }
    int any = 0;
    for (int r = 0; r < nrv; r++) {
      cb_par_t p = g.p;
      p.rv = rvs[r];
      int hid[16], hit[16];
      const int n = bruteforce(llr, &p, g.rnti, g.tried, lo, hi, hid, hit, 16);
      for (int k = 0; k < n; k++) {
        printf("HIT %s rnti=0x%04x pci=%u fmt=%s ss=%s slot=%llu mcs=%d rb=%d snr=%.1f ctrl=%d n_id=%d iters=%d rv=%d\n", g.sidecar, g.rnti,
               g.pci, g.fmt, g.ss, (unsigned long long)g.slot, g.mcs, g.nrb, g.snr, g.crc_ok, hid[k], hit[k], p.rv);
        if (!g.crc_ok) add_hit(&g, hid[k]);
      }
      any |= n > 0;
    }
    if (!any) printf("NONE %s rnti=0x%04x pci=%u fmt=%s ss=%s slot=%llu mcs=%d rb=%d snr=%.1f ctrl=%d\n", g.sidecar, g.rnti, g.pci, g.fmt, g.ss,
                     (unsigned long long)g.slot, g.mcs, g.nrb, g.snr, g.crc_ok);
    ngr++;
    fflush(stdout);
    free(llr);
  }
  printf("SUMMARY grants=%d (ctrl = CRC-pass grants: expect a hit at their tried id)\n", ngr);
  for (int i = 0; i < g_nagg; i++)
    printf("SUMMARY pci=%u fmt=%s n_id=%u hits=%d distinct_slots=%d %s\n", g_agg[i].pci, g_agg[i].fmt, g_agg[i].nid, g_agg[i].ngrant, g_agg[i].ns,
           g_agg[i].ns >= 2 ? "CONFIRMED(>=2 slots)" : "single-slot(not confirmed)");
  return 0;
}
