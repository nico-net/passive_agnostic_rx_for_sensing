/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* The transport-block abort rule (nr_cb_fail_limit.h, NR_CB_FAIL_LIMIT) on the real CPU LDPC decoder (./libldpc.so LDPCdecoder with the TB's shared
 * decode_abort_t), code block by code block in order, as the segment decoder runs them:
 *   - a TB with fewer than NR_CB_FAIL_LIMIT failed blocks is decoded to the end (every good block passes), and the TB still fails;
 *   - the NR_CB_FAIL_LIMIT-th failed block aborts the rest (not tried, failed), and the TB fails;
 *   - no failed block: every block passes (the TB passes);
 *   - set_abort(false) re-arms the counter;
 *   - a block CUT SHORT mid-decode by the TB abort is reported distinctly (NR_LDPC_ITER_CUT): no failure is noted, it is not "tried";
 *   - the parallel CPU pool: blocks decoded in ANY completion order (the order is injected by the test) through per-block abort views give,
 *     after nrLDPC_finalize_TB_order, the same tried / ok / abort count as the in-order sequential rule (the GPU rule, k_pick); the TB CRC
 *     verdict (every block ok) is unchanged.
 * The good blocks are the all-zero codeword (its CRC24B is zero) with a clean positive LLR, the bad ones pure noise.
 * "bench" [reps]: serial CPU time of the decode of a 30-block TB, old rule (abort at the first failed block) vs new, for TBs with 30, 5, 1 and 0
 * hopeless blocks among noisy-but-decodable ones. Prints NR_CB_ABORT_TEST failures=0 on success. */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "PHY/CODING/coding_defs.h"
#include "PHY/CODING/nrLDPC_defs.h"
#include "PHY/CODING/nrLDPC_coding/nrLDPC_coding_interface.h"
#include "common/config/config_userapi.h"
#include "common/utils/LOG/log.h"

configmodule_interface_t *uniqCfg = NULL;
int cpu_meas_enabled = 0; /* referenced by libldpc.so */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "%s:%d %s() %s\n", file, line, function, s ? s : "");
  exit(1);
}

enum { C = 30, Z = 384, K = 22 * 384, KPRIME = K - 24, KC = 68, NITER = 16 };
static LDPC_decoderfunc_t *g_dec;
static void (*g_finalize)(nrLDPC_TB_decoding_parameters_t *, bool); /* nrLDPC_finalize_TB_order of libldpc.so */
static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static double gauss(void)
{
  const double u = (drand48() + 1e-12), v = drand48();
  return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}
static int8_t sat(double v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : lrint(v)); }
/* mu = 0: pure noise; else the all-zero codeword's LLR mean (positive) at noise sigma */
static void make_llr(int8_t *l, double mu, double sigma)
{
  memset(l, 0, 2 * Z);                    /* punctured */
  for (int i = 2 * Z; i < KC * Z; i++)
    l[i] = sat(mu + sigma * gauss());
  for (int i = KPRIME; i < K; i++)         /* filler: known zeros */
    l[i] = 127;
}
typedef struct { int ok, tried, nfail_seen; int8_t tried_at[C], ok_at[C]; } tb_res_t;

/* decode the blocks in order with one shared abort state; kind[r]: 1 = bad (noise), 0 = good. old_rule: also abort at the first failed block (the pre-2026-10-26 behaviour). */
static tb_res_t decode_tb(const char *kind, bool old_rule, double mu_good, double sigma_good, double *us)
{
  static int8_t l[C][KC * Z + 64] __attribute__((aligned(32)));
  static uint8_t out[27000] __attribute__((aligned(32)));
  for (int r = 0; r < C; r++)
    make_llr(l[r], kind[r] ? 0 : mu_good, kind[r] ? 40 : sigma_good);
  t_nrLDPC_dec_params dp = {.check_crc = check_crc};
  dp.BG = 1;
  dp.Z = Z;
  dp.R = 13;
  dp.numMaxIter = NITER;
  dp.outMode = nrLDPC_outMode_BIT;
  dp.crc_type = CRC24_B;
  dp.Kprime = KPRIME;
  decode_abort_t ab;
  init_abort(&ab);
  set_abort(&ab, false);
  tb_res_t res = {0};
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int r = 0; r < C; r++) {
    t_nrLDPC_time_stats ts;
    memset(&ts, 0, sizeof(ts));
    const bool tried = !check_abort(&ab);
    const int it = g_dec(&dp, l[r], out, &ts, &ab);
    const bool ok = it < dp.numMaxIter;
    if (!ok && old_rule)
      set_abort(&ab, true);
    res.tried += tried;
    res.ok += ok;
    res.tried_at[r] = tried;
    res.ok_at[r] = ok;
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (us)
    *us = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
  pthread_mutex_destroy(&ab.mutex_failure);
  return res;
}

static void kinds(char *k, const int *bad, int nbad)
{
  memset(k, 0, C);
  for (int i = 0; i < nbad; i++)
    k[bad[i]] = 1;
}

static void unit(void)
{
  CHECK(NR_CB_FAIL_LIMIT == 5, "limit %d", NR_CB_FAIL_LIMIT);
  char k[C];
  /* rule on the bare counter */
  decode_abort_t ab;
  init_abort(&ab);
  for (int i = 1; i < NR_CB_FAIL_LIMIT; i++)
    CHECK(!note_cb_failure(&ab), "aborted after %d failures", i);
  CHECK(note_cb_failure(&ab), "not aborted after %d failures", NR_CB_FAIL_LIMIT);
  CHECK(check_abort(&ab), "flag");
  set_abort(&ab, false);
  CHECK(!check_abort(&ab) && !note_cb_failure(&ab), "set_abort(false) must re-arm the counter");
  CHECK(note_cb_failures(&ab, NR_CB_FAIL_LIMIT), "note_cb_failures(n) reaches the limit at once");
  pthread_mutex_destroy(&ab.mutex_failure);

  /* A: 4 failures spread out: everything is tried, the 26 good blocks pass, the TB fails */
  const int a[] = {0, 7, 13, 29};
  kinds(k, a, 4);
  tb_res_t r = decode_tb(k, false, 30, 8, NULL);
  CHECK(r.tried == C && r.ok == C - 4, "A: tried %d ok %d", r.tried, r.ok);
  for (int i = 0; i < 4; i++)
    CHECK(!r.ok_at[a[i]], "A: bad block %d passed", a[i]);
  /* B: failures at 0..4: the 5th aborts, blocks 5..29 are not tried although good */
  const int b[] = {0, 1, 2, 3, 4};
  kinds(k, b, 5);
  r = decode_tb(k, false, 30, 8, NULL);
  CHECK(r.tried == 5 && r.ok == 0, "B: tried %d ok %d", r.tried, r.ok);
  /* C: failures at 10,12,14,16,18: blocks before pass, odd ones between pass, after the 5th untried */
  const int c[] = {10, 12, 14, 16, 18};
  kinds(k, c, 5);
  r = decode_tb(k, false, 30, 8, NULL);
  CHECK(r.tried == 19 && r.ok == 14, "C: tried %d ok %d", r.tried, r.ok);
  for (int i = 0; i < C; i++)
    CHECK(r.tried_at[i] == (i <= 18) && r.ok_at[i] == (i <= 18 && !k[i]), "C: block %d tried %d ok %d", i, r.tried_at[i], r.ok_at[i]);
  /* D: one failure keeps going (the old rule stopped there) */
  const int d[] = {3};
  kinds(k, d, 1);
  r = decode_tb(k, false, 30, 8, NULL);
  CHECK(r.tried == C && r.ok == C - 1, "D new: tried %d ok %d", r.tried, r.ok);
  r = decode_tb(k, true, 30, 8, NULL);
  CHECK(r.tried == 4 && r.ok == 3, "D old rule: tried %d ok %d", r.tried, r.ok);
  /* E: no failure: the TB passes */
  kinds(k, NULL, 0);
  r = decode_tb(k, false, 30, 8, NULL);
  CHECK(r.tried == C && r.ok == C, "E: tried %d ok %d", r.tried, r.ok);
}

/* decode the blocks in the given completion order (order[0..C-1], injected) through per-block views of one TB abort object (the segment
 * decoder's mechanism), then make the result exact in block order with the library's own nrLDPC_finalize_TB_order. */
static tb_res_t decode_tb_parallel(const char *kind, const int *order, unsigned *nfail_after, bool *aborted_after, int *n_cut)
{
  static int8_t l[C][KC * Z + 64] __attribute__((aligned(32)));
  static uint8_t out[27000] __attribute__((aligned(32)));
  static nrLDPC_TB_decoding_parameters_t tb;
  static uint8_t cbuf[C * (K >> 3)];
  memset(&tb, 0, sizeof(tb));
  memset(cbuf, 0xA5, sizeof(cbuf));
  for (int r = 0; r < C; r++)
    make_llr(l[r], kind[r] ? 0 : 30, kind[r] ? 40 : 8);
  t_nrLDPC_dec_params dp = {.check_crc = check_crc};
  dp.BG = 1; dp.Z = Z; dp.R = 13; dp.numMaxIter = NITER; dp.outMode = nrLDPC_outMode_BIT; dp.crc_type = CRC24_B; dp.Kprime = KPRIME;
  decode_abort_t ab;
  init_abort(&ab);
  const bool entry = begin_block_decode(&ab);
  int cuts = 0;
  for (int i = 0; i < C; i++) {
    const int r = order[i];
    decode_abort_t blk;
    init_abort_block(&blk, &ab, r, entry);
    t_nrLDPC_time_stats ts;
    memset(&ts, 0, sizeof(ts));
    const int it = g_dec(&dp, l[r], out, &ts, &blk);
    tb.decodeTried[r] = it != (int)NR_LDPC_ITER_CUT;
    tb.decodeSuccess[r] = it < dp.numMaxIter;
    cuts += it == (int)NR_LDPC_ITER_CUT;
  }
  tb.K = K; tb.C = C; tb.abort_decode = &ab; tb.c = cbuf;
  bool rawok[C];
  memcpy(rawok, tb.decodeSuccess, sizeof(rawok));
  g_finalize(&tb, entry);
  tb_res_t res = {0};
  for (int r = 0; r < C; r++)
    if (rawok[r] && !tb.decodeSuccess[r]) /* finished by a worker after the abort point: reported failed, its bits zeroed */
      for (int b = 0; b < K >> 3; b++)
        if (cbuf[r * (K >> 3) + b] != 0) {
          CHECK(0, "block %d after the abort point not zeroed", r);
          break;
        }
  for (int r = 0; r < C; r++) {
    res.tried += tb.decodeTried[r];
    res.ok += tb.decodeSuccess[r];
    res.tried_at[r] = tb.decodeTried[r];
    res.ok_at[r] = tb.decodeSuccess[r];
  }
  if (nfail_after) *nfail_after = ab.nfail;
  if (aborted_after) *aborted_after = check_abort(&ab);
  if (n_cut) *n_cut = cuts;
  pthread_mutex_destroy(&ab.mutex_failure);
  return res;
}

static void same(const tb_res_t *a, const tb_res_t *b, const char *what)
{
  CHECK(a->tried == b->tried && a->ok == b->ok, "%s: tried %d/%d ok %d/%d", what, a->tried, b->tried, a->ok, b->ok);
  for (int i = 0; i < C; i++)
    CHECK(a->tried_at[i] == b->tried_at[i] && a->ok_at[i] == b->ok_at[i], "%s: block %d tried %d/%d ok %d/%d", what, i, a->tried_at[i],
          b->tried_at[i], a->ok_at[i], b->ok_at[i]);
}

static void order_tests(void)
{
  char k[C];
  int fwd[C], rev[C], shuf[C];
  for (int i = 0; i < C; i++) { fwd[i] = i; rev[i] = C - 1 - i; shuf[i] = (i * 11 + 3) % C; } /* 11 coprime to 30 */

  /* a block cut short by the abort is not a failed block and not tried: 5 failures are recorded at blocks 0..4, block 20 starts afterwards */
  {
    static int8_t l[KC * Z + 64] __attribute__((aligned(32)));
    static uint8_t out[27000] __attribute__((aligned(32)));
    t_nrLDPC_dec_params dp = {.check_crc = check_crc};
    dp.BG = 1; dp.Z = Z; dp.R = 13; dp.numMaxIter = NITER; dp.outMode = nrLDPC_outMode_BIT; dp.crc_type = CRC24_B; dp.Kprime = KPRIME;
    decode_abort_t ab, blk;
    init_abort(&ab);
    for (int i = 0; i < NR_CB_FAIL_LIMIT; i++) {
      init_abort_block(&blk, &ab, i, false);
      (void)note_cb_failure(&blk);
    }
    CHECK(ab.nfail == NR_CB_FAIL_LIMIT && check_abort(&ab), "5 block failures: nfail %d", ab.nfail);
    t_nrLDPC_time_stats ts;
    memset(&ts, 0, sizeof(ts));
    make_llr(l, 30, 8);
    init_abort_block(&blk, &ab, 20, false);
    int it = g_dec(&dp, l, out, &ts, &blk);
    CHECK(it == (int)NR_LDPC_ITER_CUT, "block after the abort point: it %d", it);
    CHECK(ab.nfail == NR_CB_FAIL_LIMIT, "a cut block must not count as a failure: nfail %d", ab.nfail);
    make_llr(l, 0, 40);
    it = g_dec(&dp, l, out, &ts, &blk);
    CHECK(it == (int)NR_LDPC_ITER_CUT && ab.nfail == NR_CB_FAIL_LIMIT, "cut hopeless block: it %d nfail %d", it, ab.nfail);
    /* block 2 sees only the failures BEFORE it (none): it decodes although 5 other blocks failed */
    make_llr(l, 30, 8);
    init_abort_block(&blk, &ab, 0, false);
    ab.failbits[0] = 0x3E0; /* blocks 5..9 failed */
    ab.nfail = 5;
    init_abort_block(&blk, &ab, 2, false);
    it = g_dec(&dp, l, out, &ts, &blk);
    CHECK(it < NITER, "block 2 with failures only after it must decode: it %d", it);
    init_abort_block(&blk, &ab, 12, false);
    it = g_dec(&dp, l, out, &ts, &blk);
    CHECK(it == (int)NR_LDPC_ITER_CUT, "block 12 after 5 failures: it %d", it);
    /* a TB aborted before it began: every block is cut at once */
    init_abort_block(&blk, &ab, 0, true);
    it = g_dec(&dp, l, out, &ts, &blk);
    CHECK(it == (int)NR_LDPC_ITER_CUT, "entry-aborted TB: it %d", it);
    pthread_mutex_destroy(&ab.mutex_failure);
  }

  /* pure rule: raw completion data -> block-order result */
  {
    bool rt[8] = {1, 1, 1, 1, 1, 1, 1, 1}, ro[8] = {0, 1, 0, 0, 0, 0, 1, 1}, t[8], o[8];
    unsigned nf = nr_cb_order_result(rt, ro, 8, false, t, o); /* failures at 0,2,3,4,5 -> abort after 5; blocks 6,7 finished by a worker: untried */
    CHECK(nf == 5, "order nfail %u", nf);
    for (int i = 0; i < 8; i++)
      CHECK(t[i] == (i <= 5) && o[i] == (i == 1), "order block %d tried %d ok %d", i, t[i], o[i]);
    rt[3] = false; /* rate-matching error: not tried, not a failure */
    nf = nr_cb_order_result(rt, ro, 8, false, t, o);
    CHECK(nf == 4 && !t[3] && t[6] && o[6] && t[7] && o[7], "rate-matching error block 3: nfail %u", nf);
    nf = nr_cb_order_result(rt, ro, 8, true, t, o);
    CHECK(nf == 0 && !t[0] && !o[1] && !o[6], "entry aborted: everything untried and failed");
  }

  /* the real pool mechanism, completion order injected: forward, reverse, shuffled */
  const struct { const char *name; int bad[8]; int nbad; } sc[] = {
    {"fewer than the limit", {0, 7, 13, 29}, 4},
    {"limit reached at 10, later bad blocks 12,14", {2, 4, 6, 8, 10, 12, 14}, 7},
    {"limit at the first five", {0, 1, 2, 3, 4}, 5},
    {"failures spread to the end", {3, 9, 15, 21, 27, 28, 29}, 7},
    {"no failure", {0}, 0},
  };
  for (unsigned s = 0; s < sizeof(sc) / sizeof(sc[0]); s++) {
    kinds(k, sc[s].bad, sc[s].nbad);
    tb_res_t ref = decode_tb(k, false, 30, 8, NULL);
    const int *orders[] = {fwd, rev, shuf};
    const char *on[] = {"forward", "reverse", "shuffled"};
    for (int o = 0; o < 3; o++) {
      unsigned nf;
      bool ab;
      int cuts;
      tb_res_t p = decode_tb_parallel(k, orders[o], &nf, &ab, &cuts);
      char what[96];
      snprintf(what, sizeof(what), "%s / %s", sc[s].name, on[o]);
      same(&ref, &p, what);
      CHECK(ab == (sc[s].nbad >= NR_CB_FAIL_LIMIT) && nf == (unsigned)(sc[s].nbad >= NR_CB_FAIL_LIMIT ? NR_CB_FAIL_LIMIT : sc[s].nbad),
            "%s: abort state nfail %u aborted %d", what, nf, ab);
      /* TB verdict: passes only if every block does */
      CHECK((p.ok == C) == (sc[s].nbad == 0), "%s: TB verdict", what);
      if (o == 0)
        CHECK(cuts == (sc[s].nbad >= NR_CB_FAIL_LIMIT ? C - 1 - sc[s].bad[NR_CB_FAIL_LIMIT - 1] : 0), "%s: forward order cuts %d", what, cuts);
    }
  }
}

static void bench(int reps)
{
  const int nbad[] = {30, 5, 1, 0};
  printf("BENCH 30-block TB, BG1 Z=384 K=8448 R=1/3, %d iterations max, serial LDPCdecoder CPU time per TB (us), %d reps; good blocks all-zero codeword mu=30 sigma=8\n", NITER, reps);
  for (int s = 0; s < 4; s++) {
    char k[C];
    int bad[C];
    for (int i = 0; i < nbad[s]; i++)
      bad[i] = (i * 7 + 2) % C; /* spread, distinct for nbad <= 30 since 7 is coprime to 30 */
    kinds(k, bad, nbad[s]);
    double old_us = 0, new_us = 0;
    tb_res_t ro = {0}, rn = {0};
    for (int i = 0; i < reps; i++) {
      double u;
      ro = decode_tb(k, true, 30, 8, &u);
      old_us += u;
      rn = decode_tb(k, false, 30, 8, &u);
      new_us += u;
    }
    printf("BENCH hopeless=%2d: old %8.1f us (tried %2d ok %2d)  new %8.1f us (tried %2d ok %2d)  ratio %.2f\n", nbad[s], old_us / reps, ro.tried,
           ro.ok, new_us / reps, rn.tried, rn.ok, new_us / old_us);
  }
}

int main(int argc, char **argv)
{
  logInit();
  set_glog(OAILOG_DISABLE);
  crcTableInit();
  const char *lib = getenv("NR_CB_ABORT_LDPC") ? getenv("NR_CB_ABORT_LDPC") : "./libldpc.so";
  void *h = dlopen(lib, RTLD_LAZY);
  if (!h) {
    fprintf(stderr, "%s\n", dlerror());
    return 2;
  }
  LDPC_initfunc_t *ini = (LDPC_initfunc_t *)dlsym(h, "LDPCinit");
  g_dec = (LDPC_decoderfunc_t *)dlsym(h, "LDPCdecoder");
  if (!g_dec) {
    fprintf(stderr, "no LDPCdecoder in %s\n", lib);
    return 2;
  }
  if (ini)
    ini();
  srand48(20261026);
  if (argc > 1 && !strcmp(argv[1], "bench")) {
    bench(argc > 2 ? atoi(argv[2]) : 50);
    return 0;
  }
  g_finalize = (void (*)(nrLDPC_TB_decoding_parameters_t *, bool))dlsym(h, "nrLDPC_finalize_TB_order");
  if (!g_finalize) {
    fprintf(stderr, "no nrLDPC_finalize_TB_order in %s\n", lib);
    return 2;
  }
  unit();
  order_tests();
  printf("NR_CB_ABORT_TEST failures=%d\n", g_fail);
  return g_fail != 0;
}
