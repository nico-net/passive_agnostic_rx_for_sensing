/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* MPCI C2 tests of the in-process channelizer (iq_chan) through anonymous rings. Oracle = a fresh iq_ddc fed the same blocks (iq_ddc == the frozen reference to
 * 0 LSB, test_iq_ddc_equiv), so every comparison below is "0 differing samples".
 *   test_iq_chan                       unit and thread tests
 *   test_iq_chan --soak SEC [--rate HZ] [--pace-msps X]   two cells, one channelizer thread, two consumer threads, SEC seconds, generated input, verified block by block
 *   test_iq_chan --bench [--reps N] [--secs S]            free-running single-thread capacity of the CPU backend, two cells at 122.88 and 245.76 MS/s x 4 RX */
#define _GNU_SOURCE
#include "../iq_chan.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../../openair1/PHY/TOOLS/nr_host_membudget.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); return 1; } } while (0)
#define NCH 4
#define BASE_TICK 1000000005LL /* deliberately not a multiple of the decimations */

/* ---------------------------------------------------------------- deterministic input: a noise table indexed by absolute position (random access, any block) */
static int16_t *g_tab[NCH];
static uint64_t g_T;

static void table_init(uint64_t T)
{
  g_T = T;
  for (int a = 0; a < NCH; ++a) {
    g_tab[a] = malloc((size_t)T * 4);
    uint32_t r = 0x9e3779b9u * (uint32_t)(a + 1) + 12345u;
    for (uint64_t i = 0; i < 2 * T; ++i) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; g_tab[a][i] = (int16_t)((int)(r % 6001) - 3000); }
  }
}

static void fill(int ch, uint64_t pos, int n, int16_t *dst)
{
  uint64_t i = pos % g_T;
  int done = 0;
  while (done < n) {
    const uint64_t k = (uint64_t)(n - done) < g_T - i ? (uint64_t)(n - done) : g_T - i;
    memcpy(dst + 2 * done, g_tab[ch] + 2 * i, k * 4);
    done += (int)k; i = 0;
  }
}

static void produce(shm_iq_ring_t *r, uint64_t pos, int n, int16_t *scratch[NCH])
{
  void *b[NCH];
  for (int a = 0; a < NCH; ++a) { fill(a, pos, n, scratch[a]); b[a] = scratch[a]; }
  shm_iq_ring_write(r, BASE_TICK + (int64_t)pos, b, n, NCH);
}

/* ---------------------------------------------------------------- oracle */
typedef struct { iq_ddc_t d[NCH]; int decim; } oracle_t;

static int oracle_init(oracle_t *o, double rate, int decim, double passband, double shift)
{
  o->decim = decim;
  for (int a = 0; a < NCH; ++a) {
    if (iq_ddc_init(&o->d[a], rate, decim, passband, IQ_CHAN_DEFAULT_ATTEN_DB)) return -1;
    iq_ddc_set_shift(&o->d[a], shift);
  }
  return 0;
}
static void oracle_free(oracle_t *o) { for (int a = 0; a < NCH; ++a) iq_ddc_free(&o->d[a]); }

/* run the oracle over `nb` blocks starting at input position pos0 and compare with ring samples [idx0, idx0 + nb * block / decim) of `ring` (read through a view).
 * Returns the number of differing samples (-1: could not read). */
static long compare_run(oracle_t *o, shm_iq_ring_t *ring, uint64_t idx0, uint64_t pos0, int block, int nb)
{
  const int m = block / o->decim;
  int16_t *want[NCH], *got[NCH], *in = malloc((size_t)block * 4);
  for (int a = 0; a < NCH; ++a) { want[a] = malloc((size_t)m * 4); got[a] = malloc((size_t)m * 4); }
  long bad = 0;
  for (int b = 0; b < nb && bad >= 0; ++b) {
    for (int a = 0; a < NCH; ++a) {
      fill(a, pos0 + (uint64_t)b * (uint64_t)block, block, in);
      iq_ddc_process(&o->d[a], in, block, want[a]);
    }
    if (shm_iq_ring_copy(ring, idx0 + (uint64_t)b * (uint64_t)m, m, NCH, got)) { bad = -1; break; }
    for (int a = 0; a < NCH; ++a) bad += memcmp(want[a], got[a], (size_t)m * 4) != 0 ? 1 : 0; /* blocks (channels) that differ */
  }
  for (int a = 0; a < NCH; ++a) { free(want[a]); free(got[a]); }
  free(in);
  return bad;
}

static double pb_of(double rate, int decim) { return decim == 1 ? 0.4 * rate : 0.5 * rate / decim * 0.8; }

static int16_t *g_scr[NCH];
static void scratch_init(int max_block) { for (int a = 0; a < NCH; ++a) g_scr[a] = malloc((size_t)max_block * 4); }

/* ---------------------------------------------------------------- T1: decim 1/2/4/8 x shifts 0 / -19.81 / +7.3 MHz at 122.88 and 245.76 MS/s, block sizes off the 4096 grid */
static int test_equiv(double rate, int block, int nb, int scalar)
{
  const int decs[4] = {1, 2, 4, 8};
  const double shifts[3] = {0, -19.81e6, 7.3e6};
  shm_iq_ring_t in, view[12];
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 20, rate, 3.6e9, 2) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = block};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  int id[12];
  for (int d = 0; d < 4; ++d)
    for (int s = 0; s < 3; ++s) {
      iq_chan_entry_t e = {.shift_hz = shifts[s], .decim = decs[d], .passband_hz = pb_of(rate, decs[d])};
      id[d * 3 + s] = iq_chan_cell_add(c, &e);
      CHECK(id[d * 3 + s] >= 0, "add %d", id[d * 3 + s]);
      CHECK(shm_iq_ring_view(&view[d * 3 + s], iq_chan_cell_ring(c, id[d * 3 + s])) == 0, "view");
    }
  iq_ddc_set_force_scalar(scalar);
  iq_chan_seek(c, 0);
  for (int b = 0; b < nb; ++b) produce(&in, (uint64_t)b * (uint64_t)block, block, g_scr);
  for (int b = 0; b < nb; ++b) CHECK(iq_chan_step(c) == 1, "step %d", b);
  CHECK(iq_chan_step(c) == 0, "idle after the last block");
  iq_ddc_set_force_scalar(0);
  long total_bad = 0;
  for (int d = 0; d < 4; ++d)
    for (int s = 0; s < 3; ++s) {
      oracle_t o;
      CHECK(oracle_init(&o, rate, decs[d], pb_of(rate, decs[d]), shifts[s]) == 0, "oracle");
      const long bad = compare_run(&o, &view[d * 3 + s], 0, 0, block, nb);
      CHECK(bad == 0, "rate %.2f block %d decim %d shift %.0f: %ld differing block-channels", rate / 1e6, block, decs[d], shifts[s], bad);
      const shm_iq_hdr_t *h = view[d * 3 + s].hdr;
      CHECK(h->base_tick == (BASE_TICK / decs[d]) && atomic_load(&h->head) == (uint64_t)nb * (uint64_t)(block / decs[d]) && atomic_load(&h->gaps) == 0, "timestamps decim %d base %lld head %llu gaps %llu",
            decs[d], (long long)h->base_tick, (unsigned long long)atomic_load(&h->head), (unsigned long long)atomic_load(&h->gaps));
      CHECK(fabs(h->rate - rate / decs[d]) < 1e-6 && fabs(shm_iq_ring_center(&view[d * 3 + s]) - (3.6e9 + shifts[s])) < 1e-3, "out ring rate/centre");
      oracle_free(&o);
      total_bad += bad;
    }
  /* the zero-shift decim-1 entry is the input, sample for sample */
  {
    int16_t *got[NCH], *want[NCH];
    for (int a = 0; a < NCH; ++a) { got[a] = malloc((size_t)block * 4); want[a] = malloc((size_t)block * 4); fill(a, 0, block, want[a]); }
    CHECK(shm_iq_ring_copy(&view[0], 0, block, NCH, got) == 0, "copy");
    for (int a = 0; a < NCH; ++a) { CHECK(!memcmp(got[a], want[a], (size_t)block * 4), "zero-shift decim-1 cell differs from the input (ch %d)", a); free(got[a]); free(want[a]); }
  }
  for (int i = 0; i < 12; ++i) shm_iq_ring_detach(&view[i]);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS equiv rate=%.2f MS/s block=%d x%d blocks: 12 entries (decim 1/2/4/8 x shift 0/-19.81/+7.3 MHz) 0 differing, timestamps (base_tick+pos)/decim, %s quantiser\n", rate / 1e6, block, nb,
         scalar ? "portable" : (iq_ddc_neon_compiled() ? "NEON" : "portable (no NEON in this build)"));
  (void)total_bad;
  return 0;
}

/* ---------------------------------------------------------------- T2: live add / remove / retune at block boundaries; other entries keep bit-exact streams */
static int test_live_ops(void)
{
  const double rate = 122.88e6;
  const int block = 16384, N = 24;
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, "t_live", NCH, 1 << 20, rate, 3.6e9, 2) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = block};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t ec = {.tag = "C", .shift_hz = 7.3e6, .decim = 2, .passband_hz = pb_of(rate, 2)};
  const int idC = iq_chan_cell_add(c, &ec);
  CHECK(idC >= 0, "add C");
  shm_iq_ring_t vC, vB, vNamed;
  CHECK(shm_iq_ring_view(&vC, iq_chan_cell_ring(c, idC)) == 0, "view C");
  CHECK(shm_iq_ring_attach(&vNamed, "t_live_C") == 0 && vNamed.hdr == vC.hdr, "the narrow ring is reachable by name in the in-process registry (the shm_iq device path)");
  shm_iq_ring_detach(&vNamed);
  iq_chan_seek(c, 0);
  for (int b = 0; b < N; ++b) produce(&in, (uint64_t)b * (uint64_t)block, block, g_scr);
  oracle_t oC, oB, oB2;
  CHECK(oracle_init(&oC, rate, 2, pb_of(rate, 2), 7.3e6) == 0, "oC");
  /* blocks 0..2: C alone */
  for (int b = 0; b < 3; ++b) CHECK(iq_chan_step(c) == 1, "step");
  /* add B at block 3 */
  iq_chan_entry_t eb = {.tag = "B", .shift_hz = -19.81e6, .decim = 4, .passband_hz = pb_of(rate, 4)};
  const int idB = iq_chan_cell_add(c, &eb);
  CHECK(idB >= 0 && idB != idC, "add B");
  CHECK(shm_iq_ring_view(&vB, iq_chan_cell_ring(c, idB)) == 0, "view B");
  for (int b = 3; b < 6; ++b) CHECK(iq_chan_step(c) == 1, "step");
  CHECK(compare_run(&oC, &vC, 0, 0, block, 6) == 0, "C bit-exact across the live add of B (blocks 0..5)");
  CHECK(oracle_init(&oB, rate, 4, pb_of(rate, 4), -19.81e6) == 0, "oB");
  CHECK(compare_run(&oB, &vB, 0, 3 * (uint64_t)block, block, 3) == 0, "B bit-exact from its first block (a fresh filter at block 3)");
  CHECK(vB.hdr->base_tick == (BASE_TICK + 3 * block) / 4, "B first tick %lld", (long long)vB.hdr->base_tick);
  /* remove C at block 6 */
  CHECK(iq_chan_cell_remove(c, idC) == 0 && iq_chan_cell_remove(c, idC) == -ENOENT, "remove C once");
  const uint64_t headC = atomic_load(&vC.hdr->head);
  for (int b = 6; b < 8; ++b) CHECK(iq_chan_step(c) == 1, "step");
  CHECK(atomic_load(&vC.hdr->head) == headC, "a removed entry's ring is not written any more");
  CHECK(compare_run(&oB, &vB, 3 * (uint64_t)(block / 4), 6 * (uint64_t)block, block, 2) == 0, "B bit-exact across the live removal of C (blocks 6..7)");
  /* retune B (shift) at block 8: only B's history is reset, its ring starts a new run */
  const uint64_t headB = atomic_load(&vB.hdr->head), epB = atomic_load(&vB.hdr->rf_epoch);
  CHECK(iq_chan_cell_retune(c, idB, 7.3e6, 0) == 0, "retune B");
  for (int b = 8; b < 10; ++b) CHECK(iq_chan_step(c) == 1, "step");
  CHECK(atomic_load(&vB.hdr->rf_epoch) == epB + 1 && atomic_load(&vB.hdr->valid_from) == headB && fabs(shm_iq_ring_center(&vB) - (3.6e9 + 7.3e6)) < 1e-3, "B ring: epoch bumped, new run at the retune point, new centre");
  CHECK(oracle_init(&oB2, rate, 4, pb_of(rate, 4), 7.3e6) == 0, "oB2");
  CHECK(compare_run(&oB2, &vB, headB, 8 * (uint64_t)block, block, 2) == 0, "B after the retune equals a fresh filter at the new shift started at block 8");
  CHECK(iq_chan_cell_retune(c, 15, 0, 0) == -ENOENT, "retune of an unused id");
  /* bad input and slots */
  iq_chan_entry_t bad = {.decim = 3, .passband_hz = 1e6};
  CHECK(iq_chan_cell_add(c, &bad) == -EINVAL, "decim 3 refused");
  bad.decim = 4; bad.passband_hz = 0;
  CHECK(iq_chan_cell_add(c, &bad) == -EINVAL, "decim 4 without a passband refused");
  oracle_free(&oC); oracle_free(&oB); oracle_free(&oB2);
  shm_iq_ring_detach(&vC); shm_iq_ring_detach(&vB);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS live add / remove / retune: other entries stay bit-exact, the new run starts a fresh filter, rings bump their epoch\n");
  return 0;
}


/* ---------------------------------------------------------------- T2b: budget, slots, input checks */
static int test_limits(void)
{
  const double rate = 122.88e6;
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 20, rate, 3.6e9, 0) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = 16384, .out_ring_budget_bytes = 3 * shm_iq_ring_bytes(NCH, 1 << 17)};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t e = {.shift_hz = 1e6, .decim = 2, .passband_hz = pb_of(rate, 2)};
  int ids[IQ_CHAN_MAX_CELLS], n = 0, rc;
  while ((rc = iq_chan_cell_add(c, &e)) >= 0 && n < IQ_CHAN_MAX_CELLS) ids[n++] = rc;
  CHECK(rc == -ENOMEM && n >= 1 && n < IQ_CHAN_MAX_CELLS, "the narrow-ring budget refuses the entry that does not fit: %d served, last rc %d", n, rc);
  CHECK(iq_chan_cell_remove(c, ids[0]) == 0, "remove");
  CHECK(iq_chan_cell_add(c, &e) >= 0, "the removed entry's budget is returned");
  iq_chan_destroy(c);
  iq_chan_cfg_t cfg2 = {.in = &in, .block = 16384};
  c = iq_chan_create(&cfg2);
  CHECK(c, "create 2");
  e.decim = 1; e.shift_hz = 0;
  for (n = 0; n < IQ_CHAN_MAX_CELLS; ++n) CHECK(iq_chan_cell_add(c, &e) >= 0, "add %d", n);
  CHECK(iq_chan_cell_add(c, &e) == -ENOSPC, "17th entry refused");
  iq_chan_cfg_t cfg3 = {.in = &in, .block = 12};
  CHECK(!iq_chan_create(&cfg3) && errno == EINVAL, "block not a multiple of 16 refused");
  cfg3.block = 1 << 20;
  CHECK(!iq_chan_create(&cfg3), "block above half the ring refused");
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS limits: narrow-ring budget, 16 entries, bad input\n");
  return 0;
}

/* ---------------------------------------------------------------- T2c: producer retune (rf_epoch): absolute-centre entries follow, all histories reset, rings start a new run */
static int test_ring_epoch(void)
{
  const double rate = 122.88e6;
  const int block = 16384;
  shm_iq_ring_t in, vR, vD;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 20, rate, 3.6e9, 0) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = block};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t er = {.shift_hz = 7.3e6, .decim = 2, .passband_hz = pb_of(rate, 2)};
  iq_chan_entry_t ed = {.centre_hz = 3.6e9 - 19.81e6, .decim = 4, .passband_hz = pb_of(rate, 4)};
  const int idR = iq_chan_cell_add(c, &er), idD = iq_chan_cell_add(c, &ed);
  CHECK(idR >= 0 && idD >= 0 && shm_iq_ring_view(&vR, iq_chan_cell_ring(c, idR)) == 0 && shm_iq_ring_view(&vD, iq_chan_cell_ring(c, idD)) == 0, "add");
  iq_chan_seek(c, 0);
  for (int b = 0; b < 4; ++b) { produce(&in, (uint64_t)b * (uint64_t)block, block, g_scr); CHECK(iq_chan_step(c) == 1, "step"); }
  oracle_t oR0, oD0, oR1, oD1;
  CHECK(oracle_init(&oR0, rate, 2, pb_of(rate, 2), 7.3e6) == 0 && oracle_init(&oD0, rate, 4, pb_of(rate, 4), -19.81e6) == 0, "oracles");
  CHECK(compare_run(&oR0, &vR, 0, 0, block, 4) == 0 && compare_run(&oD0, &vD, 0, 0, block, 4) == 0, "before the epoch");
  const uint64_t hR = atomic_load(&vR.hdr->head), hD = atomic_load(&vD.hdr->head), eR = atomic_load(&vR.hdr->rf_epoch), eD = atomic_load(&vD.hdr->rf_epoch);
  shm_iq_ring_new_epoch(&in, 3.6e9 + 1e6); /* the radio retunes by +1 MHz on a continuous clock */
  for (int b = 4; b < 8; ++b) { produce(&in, (uint64_t)b * (uint64_t)block, block, g_scr); CHECK(iq_chan_step(c) == 1, "step"); }
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(st.epochs == 1 && st.overruns == 0 && st.discont == 0, "stats epochs %llu", (unsigned long long)st.epochs);
  CHECK(atomic_load(&vR.hdr->rf_epoch) == eR + 1 && atomic_load(&vD.hdr->rf_epoch) == eD + 1 && atomic_load(&vR.hdr->valid_from) == hR && atomic_load(&vD.hdr->valid_from) == hD,
        "both rings start a new run at the epoch");
  CHECK(fabs(shm_iq_ring_center(&vD) - (3.6e9 - 19.81e6)) < 1e-3 && fabs(shm_iq_ring_center(&vR) - (3.6e9 + 1e6 + 7.3e6)) < 1e-3, "centres: absolute entry keeps its cell, relative entry moves with the ring");
  CHECK(oracle_init(&oR1, rate, 2, pb_of(rate, 2), 7.3e6) == 0 && oracle_init(&oD1, rate, 4, pb_of(rate, 4), -20.81e6) == 0, "oracles 1");
  CHECK(compare_run(&oR1, &vR, hR, 4 * (uint64_t)block, block, 4) == 0, "relative-shift entry: fresh filter at the epoch");
  CHECK(compare_run(&oD1, &vD, hD, 4 * (uint64_t)block, block, 4) == 0, "absolute-centre entry: new shift -20.81 MHz, fresh filter at the epoch");
  oracle_free(&oR0); oracle_free(&oD0); oracle_free(&oR1); oracle_free(&oD1);
  shm_iq_ring_detach(&vR); shm_iq_ring_detach(&vD);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS ring epoch: absolute-centre entry follows the ring retune, histories reset, no re-sync of the loop\n");
  return 0;
}

/* ---------------------------------------------------------------- T3: overrun, discontinuity: skip, history reset, entries/designs/rings/consumers kept, bit-exact resume */
static int test_overrun_recover(void)
{
  const double rate = 122.88e6;
  const int block = 16384;
  shm_iq_ring_t in, vA, vP;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 18, rate, 3.6e9, 2) == 0, "ring"); /* 16 blocks */
  iq_chan_cfg_t cfg = {.in = &in, .block = block};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t ea = {.shift_hz = -19.81e6, .decim = 2, .passband_hz = pb_of(rate, 2)}, ep = {.shift_hz = 0, .decim = 1, .passband_hz = 0.4 * rate};
  const int idA = iq_chan_cell_add(c, &ea), idP = iq_chan_cell_add(c, &ep);
  CHECK(idA >= 0 && idP >= 0, "add");
  shm_iq_ring_t *ringA = iq_chan_cell_ring(c, idA);
  CHECK(shm_iq_ring_view(&vA, ringA) == 0 && shm_iq_ring_view(&vP, iq_chan_cell_ring(c, idP)) == 0, "views");
  const int cons = shm_iq_ring_consumer_register(&vA); /* a consumer of the narrow ring registered before the overrun must survive it */
  CHECK(cons == 0, "narrow ring consumer");
  iq_chan_seek(c, 0);
  uint64_t p = 0;
  for (int b = 0; b < 3; ++b, p += (uint64_t)block) { produce(&in, p, block, g_scr); CHECK(iq_chan_step(c) == 1, "step"); }
  oracle_t o0, o1;
  CHECK(oracle_init(&o0, rate, 2, pb_of(rate, 2), -19.81e6) == 0 && oracle_init(&o1, rate, 2, pb_of(rate, 2), -19.81e6) == 0, "oracles");
  CHECK(compare_run(&o0, &vA, 0, 0, block, 3) == 0, "before the overrun");
  /* the producer runs 40 blocks ahead: the chan's position is overwritten (ring = 16 blocks) */
  for (int b = 0; b < 40; ++b, p += (uint64_t)block) produce(&in, p, block, g_scr);
  CHECK(iq_chan_step(c) == 0, "the overwritten position is skipped, not processed");
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(st.overruns == 1 && st.discont == 0 && st.dropped_in > 0 && iq_chan_cell_ring(c, idA) == ringA && st.n_cells == 2, "overrun counted once, entries kept");
  CHECK(shm_iq_ring_overruns(&in) == 1 && shm_iq_ring_consumer_overruns(&in, 0) == 1, "accounted on the wideband ring's consumer table");
  const uint64_t resume = iq_chan_pos(c);
  CHECK(resume > 3 * (uint64_t)block && resume % 16 == 0 && resume <= p, "resume position %llu", (unsigned long long)resume);
  const uint64_t headA = atomic_load(&vA.hdr->head);
  /* the producer goes on; catch up block by block until its end */
  for (int b = 0; b < 3; ++b, p += (uint64_t)block) produce(&in, p, block, g_scr);
  int got = 0;
  while (iq_chan_step(c) == 1) ++got;
  CHECK(got == 3, "recovered: %d blocks after the skip", got);
  CHECK(atomic_load(&vA.hdr->gaps) == 1 && atomic_load(&vA.hdr->valid_from) == headA + (resume - 3 * (uint64_t)block) / 2, "consumers of the narrow ring see exactly one forward gap (valid_from %llu)", (unsigned long long)atomic_load(&vA.hdr->valid_from));
  CHECK(compare_run(&o1, &vA, atomic_load(&vA.hdr->valid_from), resume, block, got) == 0, "after the overrun the stream is bit-exact with a fresh filter started at the resume position (history reset, design kept)");
  CHECK(shm_iq_ring_consumer_overruns(&vA, 0) == 0 && atomic_load(&vA.hdr->cons[0].active) == 1, "the narrow ring's consumer slot survived");
  /* discontinuity: the producer jumps forward in time (radio gap); the chan is caught up, so the new run starts exactly at its position */
  const uint64_t pos_before = iq_chan_pos(c);
  p = pos_before + 16 * (uint64_t)block / 4; /* skip some samples (multiple of 16) */
  const uint64_t jump_to = p;
  produce(&in, p, block, g_scr);
  /* a forward tick gap sets valid_from = new index; the chan is at the old head, below valid_from */
  CHECK(iq_chan_step(c) == 0, "discontinuity skip");
  iq_chan_stats(c, &st);
  CHECK(st.discont == 1 && st.overruns == 1 && iq_chan_pos(c) == jump_to, "discont counted (pos %llu want %llu)", (unsigned long long)iq_chan_pos(c), (unsigned long long)jump_to);
  CHECK(iq_chan_step(c) == 1, "resume after the discontinuity");
  oracle_free(&o0); oracle_free(&o1);
  shm_iq_ring_detach(&vA); shm_iq_ring_detach(&vP);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS overrun / discontinuity: skip, history reset, designs + rings + consumers kept, bit-exact resume\n");
  return 0;
}

/* ---------------------------------------------------------------- T5: backend registry, selection hook, capacity */
static atomic_int g_fake_runs;
static int fake_run(void *ctx, const int16_t *const *in, int n, int16_t *const *out)
{
  atomic_fetch_add(&g_fake_runs, 1);
  return iq_chan_backend_cpu.run(ctx, in, n, out);
}
static size_t fake_cap(const iq_chan_load_t *l) { return (size_t)(l->in_rate * 7); }
static iq_chan_backend_t g_fake;
static const iq_chan_backend_t *g_selected;
static const iq_chan_backend_t *fake_selector(const iq_chan_load_t *l, void *user)
{
  (void)user;
  g_selected = l && l->n_cells >= 2 ? iq_chan_backend_find("fake") : NULL; /* a GPU path would decide here from nr_gpu_path */
  return g_selected;
}

static int test_backends(void)
{
  g_fake = iq_chan_backend_cpu;
  g_fake.name = "fake"; g_fake.run = fake_run; g_fake.capacity = fake_cap;
  CHECK(iq_chan_backend_find("cpu") == &iq_chan_backend_cpu && !iq_chan_backend_find("fake"), "registry");
  CHECK(iq_chan_backend_register(&g_fake) == 0 && iq_chan_backend_register(&g_fake) == -1 && iq_chan_backend_find("fake") == &g_fake, "register once");
  iq_chan_load_t one = {.in_rate = 122.88e6, .nch = NCH, .n_cells = 1}, two = one;
  one.cell[0].decim = 2; two.n_cells = 2; two.cell[0].decim = 2; two.cell[1].decim = 4;
  CHECK(iq_chan_backend_select(&two) == &iq_chan_backend_cpu, "default selection is the CPU backend");
  iq_chan_set_selector(fake_selector, NULL);
  CHECK(iq_chan_backend_select(&one) == &iq_chan_backend_cpu && iq_chan_backend_select(&two) == &g_fake, "the selection hook decides");
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, NULL, NCH, 1 << 20, 122.88e6, 3.6e9, 0) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = 16384, .load_hint = &two};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t e = {.shift_hz = 5e6, .decim = 2, .passband_hz = pb_of(122.88e6, 2)};
  CHECK(iq_chan_cell_add(c, &e) >= 0, "add");
  iq_chan_seek(c, 0);
  produce(&in, 0, 16384, g_scr);
  CHECK(iq_chan_step(c) == 1, "step");
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(!strcmp(st.backend, "fake") && atomic_load(&g_fake_runs) == 1, "the channelizer ran on the backend the hook chose");
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  iq_chan_set_selector(NULL, NULL);
  CHECK(iq_chan_backend_select(&two) == &iq_chan_backend_cpu, "default restored");
  CHECK(g_fake.capacity(&one) == (size_t)(122.88e6 * 7), "backend capacity() is the backend's own");
  /* the CPU backend measures itself on this host */
  iq_chan_bench_t b;
  CHECK(iq_chan_cpu_bench(&two, 0, 0.2, &b) == 0 && b.samples_per_s > 1e6 && b.rt_factor > 0.05, "bench");
  const size_t cap = iq_chan_backend_cpu.capacity(&two);
  CHECK(cap > 1000000, "cpu capacity() %zu", cap);
  printf("PASS backends: registry, selection hook, backend capacity; CPU self-bench 2 cells at 122.88 MS/s x%d RX: %.1f MS/s (%.2fx real time) [Bench, this host, shared, n=1, %.2f s; logic check only]\n", NCH,
         b.samples_per_s / 1e6, b.rt_factor, b.seconds);
  return 0;
}

/* ---------------------------------------------------------------- T6: the thread, with live add / remove / retune while it runs */
typedef struct { iq_chan_t *chan; shm_iq_ring_t *in; int block; uint64_t blocks; atomic_ullong done; atomic_int stop; long pace_ns; } prod_t;
static void *producer_thread(void *p)
{
  prod_t *a = p;
  int16_t *scr[NCH];
  for (int i = 0; i < NCH; ++i) scr[i] = malloc((size_t)a->block * 4);
  for (uint64_t b = 0; b < a->blocks && !atomic_load(&a->stop); ++b) {
    /* the test is not about overruns here (sanitizer builds are slow): never run more than 32 blocks ahead of the channelizer */
    while (b * (uint64_t)a->block > iq_chan_pos(a->chan) + 32 * (uint64_t)a->block && !atomic_load(&a->stop)) { struct timespec w = {0, 200000}; nanosleep(&w, NULL); }
    produce(a->in, b * (uint64_t)a->block, a->block, scr);
    atomic_store(&a->done, b + 1);
    struct timespec w = {0, a->pace_ns};
    nanosleep(&w, NULL);
  }
  for (int i = 0; i < NCH; ++i) free(scr[i]);
  return NULL;
}

static void wait_pos(iq_chan_t *c, uint64_t pos)
{
  for (int i = 0; i < 20000 && iq_chan_pos(c) < pos; ++i) { struct timespec w = {0, 500000}; nanosleep(&w, NULL); }
}

/* input position of narrow ring index j (the ring's tick mapping is base_tick + index; pos = tick * decim - (BASE - BASE % decim)) */
static uint64_t pos_of_index(const shm_iq_ring_t *r, int decim, uint64_t j)
{
  return (uint64_t)((atomic_load(&r->hdr->tick_offset) + (int64_t)j) * decim - (BASE_TICK - BASE_TICK % decim));
}

static int test_thread_live(void)
{
  const double rate = 122.88e6;
  const int block = 16384;
  const uint64_t NB = 700;
  shm_iq_ring_t in, vC, vB, vD;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, "t_thr", NCH, 1 << 21, rate, 3.6e9, 2) == 0, "ring");
  iq_chan_cfg_t cfg = {.in = &in, .block = block};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  iq_chan_entry_t ec = {.shift_hz = 7.3e6, .decim = 2, .passband_hz = pb_of(rate, 2)};
  const int idC = iq_chan_cell_add(c, &ec);
  CHECK(idC >= 0 && shm_iq_ring_view(&vC, iq_chan_cell_ring(c, idC)) == 0, "add C");
  iq_chan_seek(c, 0);
  CHECK(iq_chan_start(c, NULL, 0) == 0, "start");
  prod_t pa = {.chan = c, .in = &in, .block = block, .blocks = NB, .pace_ns = 500000}; /* 0.5 ms per 0.133 ms block: 3.75x slower than real time, safe under ASAN too */
  pthread_t th;
  pthread_create(&th, NULL, producer_thread, &pa);
  /* live ops from this thread while the channelizer thread runs */
  wait_pos(c, 100 * (uint64_t)block);
  iq_chan_entry_t eb = {.shift_hz = -19.81e6, .decim = 4, .passband_hz = pb_of(rate, 4)};
  const int idB = iq_chan_cell_add(c, &eb);
  CHECK(idB >= 0 && shm_iq_ring_view(&vB, iq_chan_cell_ring(c, idB)) == 0, "live add B");
  wait_pos(c, 250 * (uint64_t)block);
  CHECK(iq_chan_cell_remove(c, idB) == 0, "live remove B");
  iq_chan_entry_t ed = {.centre_hz = 3.6e9 + 2.5e6, .decim = 8, .passband_hz = pb_of(rate, 8)};
  const int idD = iq_chan_cell_add(c, &ed);
  CHECK(idD >= 0 && shm_iq_ring_view(&vD, iq_chan_cell_ring(c, idD)) == 0, "live add D");
  wait_pos(c, 400 * (uint64_t)block);
  {
    /* run 1 of D (shift = centre - ring centre = +2.5 MHz) as far as it got, before the retune is issued */
    const uint64_t nb1 = atomic_load(&vD.hdr->head) / (uint64_t)(block / 8);
    oracle_t o;
    CHECK(nb1 > 0 && oracle_init(&o, rate, 8, pb_of(rate, 8), 2.5e6) == 0, "oD run 1");
    CHECK(compare_run(&o, &vD, 0, pos_of_index(&vD, 8, 0), block, (int)nb1) == 0, "D run 1 (absolute centre -> shift +2.5 MHz), %llu blocks", (unsigned long long)nb1);
    oracle_free(&o);
  }
  CHECK(iq_chan_cell_retune(c, idD, 0, 3.6e9 - 11.5e6) == 0, "live retune D");
  const uint64_t d_retune_head = atomic_load(&vD.hdr->head);
  pthread_join(th, NULL);
  wait_pos(c, NB * (uint64_t)block);
  iq_chan_stop(c);
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  CHECK(st.overruns == 0 && st.discont == 0 && st.blocks == NB, "no overrun with a paced producer: blocks %llu overruns %llu", (unsigned long long)st.blocks, (unsigned long long)st.overruns);
  /* C: untouched by every live operation, bit-exact over the whole run */
  oracle_t oC, oB, oD1;
  CHECK(oracle_init(&oC, rate, 2, pb_of(rate, 2), 7.3e6) == 0, "oC");
  CHECK(atomic_load(&vC.hdr->gaps) == 0 && atomic_load(&vC.hdr->head) == NB * (uint64_t)(block / 2), "C head");
  CHECK(compare_run(&oC, &vC, 0, 0, block, (int)NB) == 0, "C bit-exact over all %llu blocks while B and D were added / removed / retuned", (unsigned long long)NB);
  /* B: from its first block to its removal */
  const uint64_t headB = atomic_load(&vB.hdr->head);
  const uint64_t posB = pos_of_index(&vB, 4, 0);
  CHECK(posB >= 100 * (uint64_t)block && posB % (uint64_t)block == 0 && headB > 0, "B started at pos %llu", (unsigned long long)posB);
  CHECK(oracle_init(&oB, rate, 4, pb_of(rate, 4), -19.81e6) == 0, "oB");
  CHECK(compare_run(&oB, &vB, 0, posB, block, (int)(headB / (uint64_t)(block / 4))) == 0, "B bit-exact from its (live) first block until its removal; %llu blocks", (unsigned long long)(headB / (uint64_t)(block / 4)));
  /* D: run 1 until the retune, run 2 (new shift) from valid_from */
  const uint64_t vfD = atomic_load(&vD.hdr->valid_from);
  CHECK(vfD >= d_retune_head && vfD % (uint64_t)(block / 8) == 0 && atomic_load(&vD.hdr->rf_epoch) == 1, "D new run at index %llu (head at the call %llu)", (unsigned long long)vfD, (unsigned long long)d_retune_head);
  CHECK(oracle_init(&oD1, rate, 8, pb_of(rate, 8), -11.5e6) == 0, "oD1");
  /* run 2: shift = centre - ring centre = (3.6e9 - 11.5e6) - 3.6e9 = -11.5 MHz */
  const uint64_t pos2 = pos_of_index(&vD, 8, vfD), headD = atomic_load(&vD.hdr->head);
  CHECK(compare_run(&oD1, &vD, vfD, pos2, block, (int)((headD - vfD) / (uint64_t)(block / 8))) == 0, "D after its live retune: fresh filter at the new shift, %llu blocks", (unsigned long long)((headD - vfD) / (uint64_t)(block / 8)));
  oracle_free(&oC); oracle_free(&oB); oracle_free(&oD1);
  shm_iq_ring_detach(&vC); shm_iq_ring_detach(&vB); shm_iq_ring_detach(&vD);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf("PASS thread: live add / remove / retune while the channelizer thread runs (%llu blocks, 0 overruns, C bit-exact throughout)\n", (unsigned long long)NB);
  return 0;
}


/* ---------------------------------------------------------------- soak and bench */
static uint64_t host_ring_budget(void)
{
  FILE *f = fopen("/proc/meminfo", "r");
  char l[256];
  unsigned long long kb = 0;
  while (f && fgets(l, sizeof(l), f)) if (sscanf(l, "MemAvailable: %llu kB", &kb) == 1) break;
  if (f) fclose(f);
  return kb ? nr_host_membudget_calc((size_t)kb * 1024) / 4 : (size_t)1 << 30; /* the host-RAM plan's budget, a quarter of it for this test */
}

static void soak_cells(double rate, iq_chan_entry_t e[2])
{
  const int dA = rate > 200e6 ? 4 : 2, dB = rate > 200e6 ? 8 : 4; /* A: 106 PRB cell at 61.44 MS/s, B: 51 PRB cell at 30.72 MS/s */
  memset(e, 0, 2 * sizeof(e[0]));
  snprintf(e[0].tag, sizeof(e[0].tag), "A"); e[0].shift_hz = -19.81e6; e[0].decim = dA; e[0].passband_hz = iq_ddc_cell_passband(106, 30e3, rate / dA, 0);
  snprintf(e[1].tag, sizeof(e[1].tag), "B"); e[1].shift_hz = 7.3e6; e[1].decim = dB; e[1].passband_hz = iq_ddc_cell_passband(51, 30e3, rate / dB, 0);
}

static int bench_mode(int reps, double secs)
{
  const double rates[2] = {122.88e6, 245.76e6};
  for (int r = 0; r < 2; ++r) {
    iq_chan_entry_t e[2];
    soak_cells(rates[r], e);
    for (int cells = 1; cells <= 2; ++cells) {
      iq_chan_load_t l = {.in_rate = rates[r], .nch = NCH, .n_cells = (unsigned)cells};
      for (int k = 0; k < cells; ++k) { l.cell[k].decim = e[k].decim; l.cell[k].shifted = 1; l.cell[k].passband_hz = e[k].passband_hz; }
      double sum = 0;
      for (int n = 0; n < reps; ++n) {
        iq_chan_bench_t b;
        if (iq_chan_cpu_bench(&l, 0, secs, &b)) return 1;
        printf("BENCH in=%.2f MS/s x%d RX, %d cell(s) decim %d%s: rep %d/%d %.1f MS/s per channel = %.3fx real time (thread cpu %.2f s of %.2f s wall)\n", rates[r] / 1e6, NCH, cells, e[0].decim, cells > 1 ? (e[1].decim == 4 ? "+4" : "+8") : "", n + 1, reps, b.samples_per_s / 1e6, b.rt_factor, b.cpu_s, b.seconds);
        sum += b.rt_factor;
      }
      printf("BENCH_MEAN in=%.2f MS/s x%d RX, %d cell(s): %.3fx real time over n=%d\n", rates[r] / 1e6, NCH, cells, sum / reps, reps);
    }
  }
  return 0;
}

typedef struct {
  shm_iq_ring_t ring;
  int decim, block;
  double rate, shift, passband;
  uint64_t blocks_total;
  atomic_ullong verified;
  long mismatches;
  int err;
  atomic_int *stop;
} soak_cons_t;

static void *soak_consumer(void *p)
{
  soak_cons_t *a = p;
  oracle_t o;
  if (oracle_init(&o, a->rate, a->decim, a->passband, a->shift)) { a->err = 1; return NULL; }
  const int m = a->block / a->decim;
  int16_t *want[NCH], *got[NCH], *in = malloc((size_t)a->block * 4);
  for (int i = 0; i < NCH; ++i) { want[i] = malloc((size_t)m * 4); got[i] = malloc((size_t)m * 4); }
  const int id = shm_iq_ring_consumer_register(&a->ring);
  for (uint64_t b = 0; b < a->blocks_total || a->blocks_total == 0; ++b) {
    const uint64_t idx = b * (uint64_t)m;
    int r;
    while ((r = shm_iq_ring_copy(&a->ring, idx, m, NCH, got)) == 1) {
      if (atomic_load(a->stop) && b >= a->blocks_total && a->blocks_total) goto out;
      struct timespec w = {0, 50000};
      nanosleep(&w, NULL);
    }
    if (r < 0) { a->err = 2; fprintf(stderr, "SOAK consumer %c: overrun/discontinuity at block %llu\n", a->decim == 2 || a->decim == 4 ? 'A' : 'B', (unsigned long long)b); break; }
    for (int i = 0; i < NCH; ++i) {
      fill(i, b * (uint64_t)a->block, a->block, in);
      iq_ddc_process(&o.d[i], in, a->block, want[i]);
      a->mismatches += memcmp(want[i], got[i], (size_t)m * 4) != 0;
    }
    shm_iq_ring_consumer_update(&a->ring, id, idx + (uint64_t)m);
    atomic_fetch_add(&a->verified, 1);
  }
out:
  shm_iq_ring_consumer_release(&a->ring, id);
  for (int i = 0; i < NCH; ++i) { free(want[i]); free(got[i]); }
  free(in);
  oracle_free(&o);
  return NULL;
}

static int soak_mode(double secs, double rate, double pace_msps)
{
  const int block = IQ_CHAN_DEFAULT_BLOCK;
  iq_chan_entry_t e[2];
  soak_cells(rate, e);
  /* capacity of the backend on this host for exactly this load, alone */
  iq_chan_load_t l = {.in_rate = rate, .nch = NCH, .n_cells = 2};
  for (int k = 0; k < 2; ++k) { l.cell[k].decim = e[k].decim; l.cell[k].shifted = 1; l.cell[k].passband_hz = e[k].passband_hz; }
  iq_chan_bench_t bn;
  CHECK(iq_chan_cpu_bench(&l, block, 0.5, &bn) == 0, "bench");
  double pace = pace_msps > 0 ? pace_msps * 1e6 : (0.6 * bn.samples_per_s < rate ? 0.6 * bn.samples_per_s : rate); /* real time if the thread has 1.67x headroom, else 60 % of its idle capacity (the consumers' oracles share the memory system) */
  const uint64_t nblocks = (uint64_t)(secs * pace / block);
  const uint64_t cap = shm_iq_ring_plan_capacity(host_ring_budget(), rate, NCH, 1, SHM_IQ_RING_WIDE_TARGET_MS, SHM_IQ_RING_WIDE_FLOOR_MS);
  CHECK(cap, "wide ring does not fit the host-RAM budget");
  shm_iq_ring_t in;
  CHECK(shm_iq_ring_create(&in, SHM_IQ_BACKING_ANON, "soak_wide", NCH, cap, rate, 3.6e9, 2) == 0, "ring");
  shm_iq_ring_prefault(&in);
  iq_chan_cfg_t cfg = {.in = &in, .block = block, .out_ring_budget_bytes = host_ring_budget()};
  iq_chan_t *c = iq_chan_create(&cfg);
  CHECK(c, "create");
  int id[2];
  soak_cons_t sc[2];
  memset(sc, 0, sizeof(sc));
  atomic_int stop = 0;
  for (int k = 0; k < 2; ++k) {
    id[k] = iq_chan_cell_add(c, &e[k]);
    CHECK(id[k] >= 0, "add %d", id[k]);
    CHECK(shm_iq_ring_view(&sc[k].ring, iq_chan_cell_ring(c, id[k])) == 0, "view");
    sc[k].decim = e[k].decim; sc[k].block = block; sc[k].rate = rate; sc[k].shift = e[k].shift_hz; sc[k].passband = e[k].passband_hz; sc[k].blocks_total = nblocks; sc[k].stop = &stop;
  }
  iq_chan_seek(c, 0);
  pthread_t tc[2], tp;
  for (int k = 0; k < 2; ++k) pthread_create(&tc[k], NULL, soak_consumer, &sc[k]);
  CHECK(iq_chan_start(c, NULL, 0) == 0, "start");
  /* producer in this thread, paced */
  int16_t *scr[NCH];
  for (int i = 0; i < NCH; ++i) scr[i] = malloc((size_t)block * 4);
  const uint64_t t0 = shm_iq_now_ns();
  const double blk_ns = 1e9 * block / pace;
  uint64_t late_max = 0, late_n = 0;
  for (uint64_t b = 0; b < nblocks; ++b) {
    const uint64_t due = t0 + (uint64_t)((double)b * blk_ns), now = shm_iq_now_ns();
    if (now < due) { struct timespec w = {(time_t)((due - now) / 1000000000ull), (long)((due - now) % 1000000000ull)}; nanosleep(&w, NULL); }
    else if (now - due > 5000000) { late_n++; if (now - due > late_max) late_max = now - due; }
    produce(&in, b * (uint64_t)block, block, scr);
  }
  const double wall = (double)(shm_iq_now_ns() - t0) * 1e-9;
  for (int i = 0; i < 400 && iq_chan_pos(c) < nblocks * (uint64_t)block; ++i) { struct timespec w = {0, 25000000}; nanosleep(&w, NULL); }
  iq_chan_stop(c);
  atomic_store(&stop, 1);
  for (int k = 0; k < 2; ++k) pthread_join(tc[k], NULL);
  iq_chan_stats_t st;
  iq_chan_stats(c, &st);
  (void)tp;
  const double util = (double)st.work_ns * 1e-9 / wall;
  int rc = 0;
  for (int k = 0; k < 2; ++k) {
    const shm_iq_hdr_t *h = sc[k].ring.hdr;
    printf("SOAK cell %c: decim %d (%.2f MS/s out), %llu/%llu blocks verified bit-exact vs an independent iq_ddc, mismatches %ld, ring gaps %llu valid_from %llu head %llu (timestamps continuous), consumer overruns %llu\n",
           'A' + k, e[k].decim, rate / e[k].decim / 1e6, (unsigned long long)atomic_load(&sc[k].verified), (unsigned long long)nblocks, sc[k].mismatches, (unsigned long long)atomic_load(&h->gaps),
           (unsigned long long)atomic_load(&h->valid_from), (unsigned long long)atomic_load(&h->head), (unsigned long long)shm_iq_ring_consumer_overruns(&sc[k].ring, 0));
    if (sc[k].mismatches || sc[k].err || atomic_load(&sc[k].verified) != nblocks || atomic_load(&h->gaps) || atomic_load(&h->valid_from) || atomic_load(&h->head) != nblocks * (uint64_t)(block / e[k].decim)) rc = 1;
  }
  printf("SOAK in=%.2f MS/s x%d RX, 2 cells (decim %d+%d), %.1f s wall, %llu blocks: paced at %.2f MS/s per channel = %.3fx real time of the input rate; channelizer thread busy %.1f %% (work %.3f ms/block avg, max %.3f ms, block = %.3f ms of signal at the pace); "
         "lag max %.1f ms, longest step gap %.1f ms; overruns %llu discont %llu dropped_in %llu; producer late >5 ms: %llu (max %.2f ms); free-run bench of this load alone: %.1f MS/s = %.3fx real time\n",
         rate / 1e6, NCH, e[0].decim, e[1].decim, wall, (unsigned long long)st.blocks, pace / 1e6, pace / rate, 100 * util, 1e-6 * (double)st.work_ns / (double)(st.blocks ? st.blocks : 1), 1e-6 * (double)st.work_max_ns,
         1e3 * block / pace, 1e3 * (double)st.lag_max / rate, 1e-6 * (double)st.step_gap_max_ns, (unsigned long long)st.overruns, (unsigned long long)st.discont, (unsigned long long)st.dropped_in, (unsigned long long)late_n, 1e-6 * (double)late_max, bn.samples_per_s / 1e6, bn.rt_factor);
  if (st.overruns || st.discont || st.blocks != nblocks) rc = 1;
  for (int i = 0; i < NCH; ++i) free(scr[i]);
  for (int k = 0; k < 2; ++k) shm_iq_ring_detach(&sc[k].ring);
  iq_chan_destroy(c);
  shm_iq_ring_detach(&in);
  printf(rc ? "SOAK FAILED\n" : "SOAK OK\n");
  return rc;
}

int main(int argc, char **argv)
{
  double soak = 0, rate = 122.88e6, pace = 0, bsecs = 2;
  int bench = 0, reps = 3;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--soak") && i + 1 < argc) soak = atof(argv[++i]);
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--pace-msps") && i + 1 < argc) pace = atof(argv[++i]);
    else if (!strcmp(argv[i], "--bench")) bench = 1;
    else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--secs") && i + 1 < argc) bsecs = atof(argv[++i]);
    else { fprintf(stderr, "usage: test_iq_chan [--soak SEC [--rate HZ] [--pace-msps X]] [--bench [--reps N] [--secs S]]\n"); return 2; }
  }
  table_init(1000003);
  scratch_init(1 << 18);
  if (bench) return bench_mode(reps, bsecs);
  if (soak > 0) return soak_mode(soak, rate, pace);
  int fail = 0;
  fail |= test_equiv(122.88e6, 65536, 4, 0);
  fail |= test_equiv(245.76e6, 6160, 6, 0);
  fail |= test_equiv(245.76e6, 20496, 5, 1);
  fail |= test_live_ops();
  fail |= test_limits();
  fail |= test_ring_epoch();
  fail |= test_overrun_recover();
  fail |= test_backends();
  fail |= test_thread_live();
  printf(fail ? "FAILED\n" : "ALL OK\n");
  return fail;
}
