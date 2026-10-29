/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* mpci A7: per-cell state of the CSI-RS discovery host side (nr_csirs_disc{,_feed,_td,_fp}.c).
 *
 * Interleave tests (plan Task A7 / MPCI_CONVERT): cell A runs on input X, cell B on input Y; the same calls are made (1) solo, (2) alternating on ONE thread that re-binds
 * between calls, (3) from two threads bound with nr_cell_bind in lock-step. Every output of a cell must equal its solo run. Inputs are the synthetic CSI-RS rings of the
 * existing nr_csirs_disc_fp tests (row 1 TRS vs row 6, different period/offset/extent, different N_RB) and the random IQ of the existing TD fixture (different N_RB, FFT size, RX count).
 * Cell indices are fresh per run (statics are not re-initialised when a cell is rebound).
 *
 *   T1 detector: nr_csirs_disc_detect_candidates over a growing ring (cumulative history + group rotation) per cell.
 *   T2 TD front end: ingest duty (burst/steady windows, rate-limit duty set per cell), per-cell queue, per-cell FEP into the ring.
 *   T3 prealloc: nr_csirs_disc_cell_prealloc for two cells with different carriers: rings sized from each cell's N_RB / n_ant, host ledger = sum, workers bound (each worker
 *      drains ITS cell's TD queue), per-cell metrics, release. */
#include "nr_csirs_disc_synth.h"
#include "nr_csirs_disc_cell.h"
#include "nr_csirs_disc_td.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "PHY/TOOLS/nr_host_membudget.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include "common/config/config_userapi.h"
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

configmodule_interface_t *uniqCfg;
void exit_function(const char *file, const char *function, int line, const char *message, int fatal)
{
  fprintf(stderr, "exit_function %s:%d %s\n", file, line, message ? message : "");
  abort();
}

static int failures;
#define CHECK(c) \
  do { \
    if (!(c)) { \
      fprintf(stderr, "CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++failures; \
    } \
  } while (0)

/* ---------------------------------------------------------------- T1: detector */
enum { STEPS = 5, MAXFP = 32 };
typedef struct {
  int n[STEPS];
  nr_csirs_disc_fp_t fp[STEPS][MAXFP];
} det_result_t;
static const int written_at[STEPS] = {160, 320, 480, 640, 640};

static nr_csirs_disc_synth_resource_t resource_x(void)
{
  /* row 1 (TRS) 4:17, period 5 offset 1 : the existing fp-test fixture */
  nr_csirs_disc_synth_resource_t c = {.row = 1, .fd_bitmap = 1, .l0 = 4, .l1 = 8, .cdm = 0, .density = 3, .rb_start = 4, .n_rb = 17,
                                      .scramb_id = 415, .period = 5, .offset = 1, .power = 1.6f};
  return c;
}
static nr_csirs_disc_synth_resource_t resource_y(void)
{
  /* row 6, 8:20 PRB, period 10 offset 3 */
  nr_csirs_disc_synth_resource_t c = {.row = 6, .fd_bitmap = 15, .l0 = 4, .l1 = 8, .cdm = 1, .density = 2, .rb_start = 8, .n_rb = 20,
                                      .scramb_id = 101, .period = 10, .offset = 3, .power = 3.0f};
  return c;
}
static int make_ring(nr_csirs_disc_ring_t *r, int n_rb, const nr_csirs_disc_synth_resource_t *res, uint32_t seed)
{
  nr_csirs_disc_synth_cfg_t cfg = {.n_slots = 640, .n_rb = n_rb, .n_ant = 4, .slots_per_frame = 20, .first_slot = 1003, .seed = seed,
                                   .snr_db = 5, .load = 1, .dmrs = true, .ssb = true, .resources = res, .n_resources = 1};
  return nr_csirs_disc_synth(r, &cfg);
}

typedef struct {
  int cell;
  const nr_csirs_disc_ring_t *ring;
  det_result_t *out;
} det_ctx_t;
static void det_step(const det_ctx_t *c, int step)
{
  nr_cell_bind(c->cell);
  nr_csirs_disc_ring_t view = *c->ring;
  view.written = written_at[step];
  const nr_csirs_disc_fp_cfg_t cfg = {.min_strength = 1.03f};
  c->out->n[step] = nr_csirs_disc_detect_candidates(&view, &cfg, c->out->fp[step], MAXFP);
}
static void det_reset(int cell)
{
  nr_cell_bind(cell);
  nr_csirs_disc_detect_reset();
}
static bool same_fp(const nr_csirs_disc_fp_t *a, const nr_csirs_disc_fp_t *b)
{
  return a->sym_mask == b->sym_mask && a->sc_mask == b->sc_mask && a->density == b->density && a->rb_start == b->rb_start && a->n_rb == b->n_rb
         && a->period == b->period && a->offset == b->offset && a->strength == b->strength;
}
static bool same_det(const det_result_t *a, const det_result_t *b)
{
  for (int s = 0; s < STEPS; ++s) {
    if (a->n[s] != b->n[s]) return false;
    for (int i = 0; i < a->n[s] && i < MAXFP; ++i)
      if (!same_fp(&a->fp[s][i], &b->fp[s][i])) return false;
  }
  return true;
}

typedef struct {
  det_ctx_t ctx;
  sem_t go, done;
  int steps;
} det_thread_t;
static void *det_thread_main(void *p)
{
  det_thread_t *t = p;
  nr_cell_bind(t->ctx.cell);
  for (int s = 0; s < t->steps; ++s) {
    sem_wait(&t->go);
    det_step(&t->ctx, s);
    sem_post(&t->done);
  }
  return NULL;
}

static void test_detector(void)
{
  nr_csirs_disc_synth_init();
  nr_csirs_disc_ring_t rx = {0}, ry = {0};
  const nr_csirs_disc_synth_resource_t resx = resource_x(), resy = resource_y();
  CHECK(make_ring(&rx, 32, &resx, 0x839412) == 0);
  CHECK(make_ring(&ry, 48, &resy, 0x1d6c07) == 0);

  static det_result_t solo_x, solo_y, one_x, one_y, two_x, two_y;
  /* solo: cells 1 (X) and 2 (Y), each alone */
  det_ctx_t sx = {1, &rx, &solo_x}, sy = {2, &ry, &solo_y};
  det_reset(1);
  for (int s = 0; s < STEPS; ++s) det_step(&sx, s);
  det_reset(2);
  for (int s = 0; s < STEPS; ++s) det_step(&sy, s);
  CHECK(solo_x.n[STEPS - 1] > 0);
  CHECK(solo_y.n[STEPS - 1] > 0);
  CHECK(!same_det(&solo_x, &solo_y)); /* the two inputs must differ, else the test proves nothing */

  /* one thread, re-binding between calls (cells 3 and 4, a non-zero pair on purpose) */
  det_ctx_t ox = {3, &rx, &one_x}, oy = {4, &ry, &one_y};
  det_reset(3);
  det_reset(4);
  for (int s = 0; s < STEPS; ++s) {
    det_step(&ox, s);
    det_step(&oy, s);
  }
  CHECK(same_det(&one_x, &solo_x));
  CHECK(same_det(&one_y, &solo_y));

  /* two threads bound to their cells (cells 5 and 6), lock-step A,B,A,B */
  det_reset(5);
  det_reset(6);
  det_thread_t ta, tb;
  memset(&ta, 0, sizeof(ta));
  memset(&tb, 0, sizeof(tb));
  ta.ctx = (det_ctx_t){5, &rx, &two_x};
  tb.ctx = (det_ctx_t){6, &ry, &two_y};
  ta.steps = tb.steps = STEPS;
  sem_init(&ta.go, 0, 0);
  sem_init(&ta.done, 0, 0);
  sem_init(&tb.go, 0, 0);
  sem_init(&tb.done, 0, 0);
  pthread_t pa, pb;
  pthread_create(&pa, NULL, det_thread_main, &ta);
  pthread_create(&pb, NULL, det_thread_main, &tb);
  for (int s = 0; s < STEPS; ++s) {
    sem_post(&ta.go);
    sem_wait(&ta.done);
    sem_post(&tb.go);
    sem_wait(&tb.done);
  }
  pthread_join(pa, NULL);
  pthread_join(pb, NULL);
  CHECK(same_det(&two_x, &solo_x));
  CHECK(same_det(&two_y, &solo_y));

  /* a sibling's input must have changed the result had the history been shared: cell 7 runs Y's history on X's steps (negative control) */
  static det_result_t shared;
  det_ctx_t shx = {7, &rx, &shared};
  det_reset(7);
  nr_cell_bind(7);
  for (int s = 0; s < STEPS; ++s) {
    det_step(&shx, s);
    if (s == 1) { /* the "sibling" call on the SAME cell index = a shared history */
      det_result_t scratch;
      det_ctx_t other = {7, &ry, &scratch};
      det_step(&other, 1);
    }
  }
  CHECK(!same_det(&shared, &solo_x)); /* proves the steps are sensitive to history interleaving */
  nr_cell_bind(0);
  nr_csirs_disc_synth_free(&rx);
  nr_csirs_disc_synth_free(&ry);
}

/* ---------------------------------------------------------------- T2/T3: TD front end */
typedef struct {
  PHY_VARS_NR_UE ue;
  int fft, rb, n_ant;
} cell_ue_t;

static void ue_setup(cell_ue_t *u, int mu, int rb, int fft, int n_ant, uint32_t seed)
{
  memset(u, 0, sizeof(*u));
  u->fft = fft;
  u->rb = rb;
  u->n_ant = n_ant;
  NR_DL_FRAME_PARMS *fp = &u->ue.frame_parms;
  fp->numerology_index = mu;
  fp->slots_per_subframe = 1 << mu;
  fp->slots_per_frame = 10 << mu;
  fp->ofdm_symbol_size = fft;
  fp->nb_prefix_samples = fft * 288 / 4096;
  fp->nb_prefix_samples0 = fp->nb_prefix_samples + (64 << mu) * fft / 4096;
  fp->symbols_per_slot = 14;
  fp->samples_per_slot_wCP = 14 * fft;
  fp->samples_per_slotN0 = 14 * (fft + fp->nb_prefix_samples);
  fp->samples_per_slot0 = fp->samples_per_slotN0 + fp->nb_prefix_samples0 - fp->nb_prefix_samples;
  fp->samples_per_subframe = fp->samples_per_slotN0 * (1 << mu) + 2 * (fp->nb_prefix_samples0 - fp->nb_prefix_samples);
  fp->samples_per_frame = 10 * fp->samples_per_subframe;
  fp->nb_antennas_rx = n_ant;
  fp->N_RB_DL = rb;
  fp->first_carrier_offset = fft - 6 * rb;
  fp->ofdm_offset_divisor = 8;
  for (int l = 0; l < 14 * (1 << mu); ++l)
    fp->symbol_rotation[0][l] = (c16_t){lrint(32767 * cos(.13 * l)), lrint(32767 * sin(.13 * l))};
  for (int k = 0; k < fft; ++k)
    fp->timeshift_symbol_rotation[k] = (c16_t){lrint(32767 * cos(2 * M_PI * k * 36 / fft)), lrint(32767 * sin(2 * M_PI * k * 36 / fft))};
  u->ue.common_vars.rxdata = calloc(n_ant, sizeof(c16_t *));
  uint32_t rng = seed;
  for (int a = 0; a < n_ant; ++a) {
    u->ue.common_vars.rxdata[a] = aligned_alloc(64, 2 * fp->samples_per_frame * sizeof(c16_t));
    for (int k = 0; k < 2 * fp->samples_per_frame; ++k) {
      rng = rng * 1664525 + 1013904223;
      u->ue.common_vars.rxdata[a][k].r = (rng >> 22) - 512;
      rng = rng * 1664525 + 1013904223;
      u->ue.common_vars.rxdata[a][k].i = (rng >> 22) - 512;
    }
  }
}
static void ue_free(cell_ue_t *u)
{
  for (int a = 0; a < u->n_ant; ++a) free(u->ue.common_vars.rxdata[a]);
  free(u->ue.common_vars.rxdata);
}

enum { DUTY_SLOTS = 12000, TD_CAP = 16 };
typedef struct {
  int8_t code[DUTY_SLOTS];
} duty_t;

/* the push sequence of one cell: duty set per cell (A: steady G=1 of div 2 after the burst; B: baseline), then DUTY_SLOTS offers */
static void duty_setup(int cell, cell_ue_t *u, bool steady)
{
  nr_cell_bind(cell);
  CHECK(nr_csirs_disc_td_init(&u->ue.frame_parms, u->n_ant, TD_CAP) == 0);
  if (steady) nr_csirs_disc_td_set_duty(true, 1, 2, false);
}
static void duty_push(int cell, cell_ue_t *u, int from, int to, duty_t *out)
{
  nr_cell_bind(cell);
  for (int s = from; s < to; ++s) out->code[s] = (int8_t)nr_csirs_disc_td_push(&u->ue, (uint32_t)s, (uint16_t)(s / 20 % 1024), (uint8_t)(s % 20));
}
typedef struct {
  int cell, from, to;
  cell_ue_t *ue;
  duty_t *out;
} duty_job_t;
typedef struct {
  duty_job_t job;
  sem_t go, done;
  int chunks;
} duty_thread_t;
enum { CHUNK = 1000 };
static void *duty_thread_main(void *p)
{
  duty_thread_t *t = p;
  nr_cell_bind(t->job.cell);
  for (int c = 0; c < t->chunks; ++c) {
    sem_wait(&t->go);
    duty_push(t->job.cell, t->job.ue, c * CHUNK, (c + 1) * CHUNK, t->job.out);
    sem_post(&t->done);
  }
  return NULL;
}
static bool same_duty(const duty_t *a, const duty_t *b)
{
  return memcmp(a->code, b->code, sizeof(a->code)) == 0;
}

static void test_td(void)
{
  /* A: 4 RX, 24 PRB, FFT 512 ; B: 2 RX, 52 PRB, FFT 1024  (different N_RB / FFT / RX count) */
  static cell_ue_t ua[4], ub[4];
  for (int i = 0; i < 4; ++i) {
    ue_setup(&ua[i], 1, 24, 512, 4, 123);
    ue_setup(&ub[i], 1, 52, 1024, 2, 777);
  }
  static duty_t solo_a, solo_b, one_a, one_b, two_a, two_b;

  /* solo (cells 1, 2) */
  duty_setup(1, &ua[0], true);
  duty_push(1, &ua[0], 0, DUTY_SLOTS, &solo_a);
  duty_setup(2, &ub[0], false);
  duty_push(2, &ub[0], 0, DUTY_SLOTS, &solo_b);
  int ok_a = 0, skip_a = 0, ok_b = 0, skip_b = 0;
  for (int s = 0; s < DUTY_SLOTS; ++s) {
    ok_a += solo_a.code[s] == 0;
    skip_a += solo_a.code[s] == 1;
    ok_b += solo_b.code[s] == 0;
    skip_b += solo_b.code[s] == 1;
  }
  CHECK(ok_a > 0 && skip_a > 0 && ok_b > 0 && skip_b > 0);
  CHECK(!same_duty(&solo_a, &solo_b)); /* steady duty vs baseline differ in the steady phase */
  nr_cell_bind(1);
  CHECK(nr_csirs_disc_td_pending() == TD_CAP);
  nr_cell_bind(2);
  CHECK(nr_csirs_disc_td_pending() == TD_CAP);

  /* one thread, alternating chunks with re-binding (cells 3, 4) */
  duty_setup(3, &ua[1], true);
  duty_setup(4, &ub[1], false);
  for (int c = 0; c < DUTY_SLOTS / CHUNK; ++c) {
    duty_push(3, &ua[1], c * CHUNK, (c + 1) * CHUNK, &one_a);
    duty_push(4, &ub[1], c * CHUNK, (c + 1) * CHUNK, &one_b);
  }
  CHECK(same_duty(&one_a, &solo_a));
  CHECK(same_duty(&one_b, &solo_b));

  /* two bound threads, lock-step chunks (cells 5, 6); the duty is set from the main thread bound to the cell, as the discovery worker does */
  duty_setup(5, &ua[2], true);
  duty_setup(6, &ub[2], false);
  duty_thread_t ta, tb;
  memset(&ta, 0, sizeof(ta));
  memset(&tb, 0, sizeof(tb));
  ta.job = (duty_job_t){5, 0, 0, &ua[2], &two_a};
  tb.job = (duty_job_t){6, 0, 0, &ub[2], &two_b};
  ta.chunks = tb.chunks = DUTY_SLOTS / CHUNK;
  sem_init(&ta.go, 0, 0);
  sem_init(&ta.done, 0, 0);
  sem_init(&tb.go, 0, 0);
  sem_init(&tb.done, 0, 0);
  pthread_t pa, pb;
  pthread_create(&pa, NULL, duty_thread_main, &ta);
  pthread_create(&pb, NULL, duty_thread_main, &tb);
  for (int c = 0; c < DUTY_SLOTS / CHUNK; ++c) {
    sem_post(&ta.go);
    sem_post(&tb.go);
    sem_wait(&ta.done);
    sem_wait(&tb.done);
  }
  pthread_join(pa, NULL);
  pthread_join(pb, NULL);
  CHECK(same_duty(&two_a, &solo_a));
  CHECK(same_duty(&two_b, &solo_b));

  /* per-cell queue + FEP into the ring: push 3 slots into A and 2 into B (interleaved), drain, compare with the solo drain */
  static nr_csirs_disc_ring_t ring_solo_a, ring_solo_b, ring_il_a, ring_il_b;
  const NR_DL_FRAME_PARMS *fa = &ua[0].ue.frame_parms, *fb = &ub[0].ue.frame_parms;
  CHECK(nr_csirs_disc_ring_init(&ring_solo_a, 4, 24, fa->slots_per_frame, 8, false) == 0);
  CHECK(nr_csirs_disc_ring_init(&ring_solo_b, 2, 52, fb->slots_per_frame, 8, false) == 0);
  CHECK(nr_csirs_disc_ring_init(&ring_il_a, 4, 24, fa->slots_per_frame, 8, false) == 0);
  CHECK(nr_csirs_disc_ring_init(&ring_il_b, 2, 52, fb->slots_per_frame, 8, false) == 0);
  /* cells 11/12 solo, cells 13/14 interleaved: fresh duty state, offer slots 1..3 (A) and 1..2 (B) inside the first acquisition window */
  const int cs_a = 11, cs_b = 12, ci_a = 13, ci_b = 14;
  nr_cell_bind(cs_a);
  CHECK(nr_csirs_disc_td_init(fa, 4, TD_CAP) == 0);
  for (int s = 1; s <= 3; ++s) CHECK(nr_csirs_disc_td_push(&ua[3].ue, s, 0, s) == 0);
  CHECK(nr_csirs_disc_td_pending() == 3);
  CHECK(nr_csirs_disc_td_drain(&ring_solo_a) == 3);
  nr_cell_bind(cs_b);
  CHECK(nr_csirs_disc_td_init(fb, 2, TD_CAP) == 0);
  for (int s = 1; s <= 2; ++s) CHECK(nr_csirs_disc_td_push(&ub[3].ue, s, 0, s) == 0);
  CHECK(nr_csirs_disc_td_pending() == 2);
  CHECK(nr_csirs_disc_td_drain(&ring_solo_b) == 2);

  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_init(fa, 4, TD_CAP) == 0);
  nr_cell_bind(ci_b);
  CHECK(nr_csirs_disc_td_init(fb, 2, TD_CAP) == 0);
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_push(&ua[3].ue, 1, 0, 1) == 0);
  nr_cell_bind(ci_b);
  CHECK(nr_csirs_disc_td_push(&ub[3].ue, 1, 0, 1) == 0);
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_push(&ua[3].ue, 2, 0, 2) == 0);
  nr_cell_bind(ci_b);
  CHECK(nr_csirs_disc_td_pending() == 1);
  CHECK(nr_csirs_disc_td_push(&ub[3].ue, 2, 0, 2) == 0);
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_pending() == 2);
  CHECK(nr_csirs_disc_td_push(&ua[3].ue, 3, 0, 3) == 0);
  nr_cell_bind(ci_b);
  CHECK(nr_csirs_disc_td_drain(&ring_il_b) == 2);
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_pending() == 3);
  CHECK(nr_csirs_disc_td_drain(&ring_il_a) == 3);
  CHECK(ring_il_a.written == ring_solo_a.written && ring_il_b.written == ring_solo_b.written);
  CHECK(memcmp(ring_il_a.grid_base, ring_solo_a.grid_base, 3 * ring_solo_a.slot_stride * sizeof(int16_t)) == 0);
  CHECK(memcmp(ring_il_b.grid_base, ring_solo_b.grid_base, 2 * ring_solo_b.slot_stride * sizeof(int16_t)) == 0);
  CHECK(memcmp(ring_il_a.meta, ring_solo_a.meta, 3 * sizeof(*ring_solo_a.meta)) == 0);
  CHECK(memcmp(ring_il_b.meta, ring_solo_b.meta, 2 * sizeof(*ring_solo_b.meta)) == 0);
  CHECK(ring_solo_a.slot_stride != ring_solo_b.slot_stride);
  /* an epoch reset of one cell leaves the other cell's queue alone */
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_push(&ua[3].ue, 4, 0, 4) == 0);
  nr_cell_bind(ci_b);
  CHECK(nr_csirs_disc_td_push(&ub[3].ue, 4, 0, 4) == 0);
  nr_csirs_disc_td_reset_cell(ci_a);
  CHECK(nr_csirs_disc_td_pending() == 1); /* ci_b still holds its slot */
  nr_cell_bind(ci_a);
  CHECK(nr_csirs_disc_td_pending() == 0);

  nr_csirs_disc_ring_free(&ring_solo_a);
  nr_csirs_disc_ring_free(&ring_solo_b);
  nr_csirs_disc_ring_free(&ring_il_a);
  nr_csirs_disc_ring_free(&ring_il_b);
  for (int c = 1; c <= 14; ++c) {
    nr_cell_bind(c);
    nr_csirs_disc_td_free();
  }
  nr_cell_bind(0);
  for (int i = 0; i < 4; ++i) {
    ue_free(&ua[i]);
    ue_free(&ub[i]);
  }
}

/* ---------------------------------------------------------------- T3: prealloc, ledger, bound workers */
static bool wait_written(int cell, uint64_t want)
{
  for (int i = 0; i < 600; ++i) {
    nr_cell_bind(cell);
    nr_csirs_disc_ring_t *r = nr_csirs_disc_worker_ring();
    nr_cell_bind(0);
    if (r && __atomic_load_n(&r->written, __ATOMIC_ACQUIRE) >= want) return true;
    struct timespec d = {0, 10 * 1000 * 1000};
    nanosleep(&d, NULL);
  }
  return false;
}

static void test_prealloc(void)
{
  static cell_ue_t ua, ub;
  ue_setup(&ua, 1, 24, 512, 4, 31);
  ue_setup(&ub, 1, 52, 2048, 2, 57);
  const int ca = 8, cb = 9;
  CHECK(nr_csirs_disc_cell_prealloc(ca, &ua.ue.frame_parms, 4) == 0);
  CHECK(nr_csirs_disc_cell_prealloc(cb, &ub.ue.frame_parms, 2) == 0);
  CHECK(nr_cell_idx() == 0); /* the caller's binding is restored */
  nr_csirs_disc_cell_mem_t ma = {0}, mb = {0}, mnone = {0};
  CHECK(nr_csirs_disc_cell_mem(ca, &ma));
  CHECK(nr_csirs_disc_cell_mem(cb, &mb));
  CHECK(!nr_csirs_disc_cell_mem(10, &mnone));
  CHECK(ma.n_rb == 24 && ma.n_ant == 4 && mb.n_rb == 52 && mb.n_ant == 2);
  CHECK(ma.ring_slots > 0 && mb.ring_slots > 0 && ma.td_slots > 0 && mb.td_slots > 0);
  /* rings are sized from the cell's N_RB x n_ant: bytes per slot = n_ant * 14 * 24 * n_rb * 2 */
  CHECK(ma.ring_host_bytes == (size_t)ma.ring_slots * 4 * 14 * 24 * 24 * 2);
  CHECK(mb.ring_host_bytes == (size_t)mb.ring_slots * 2 * 14 * 24 * 52 * 2);
  CHECK(ma.td_host_bytes == nr_csirs_disc_td_bytes(&ua.ue.frame_parms, 4, ma.td_slots));
  CHECK(mb.td_host_bytes == nr_csirs_disc_td_bytes(&ub.ue.frame_parms, 2, mb.td_slots));
  CHECK(ma.td_host_bytes != mb.td_host_bytes);
  CHECK(nr_csirs_disc_cell_host_need(&ua.ue.frame_parms, 4) >= ma.ring_host_bytes + ma.td_host_bytes);
  /* host ledger: the membudget holds the SUM of the two cells (one figure per module) */
  CHECK(nr_host_membudget_committed() == ma.ring_host_bytes + ma.td_host_bytes + mb.ring_host_bytes + mb.td_host_bytes);

  /* each worker is bound to its cell: it drains ITS cell's TD queue into ITS ring */
  nr_cell_bind(ca);
  for (int s = 1; s <= 3; ++s) CHECK(nr_csirs_disc_td_push(&ua.ue, s, 0, s) == 0);
  nr_cell_bind(cb);
  for (int s = 1; s <= 5; ++s) CHECK(nr_csirs_disc_td_push(&ub.ue, s, 0, s) == 0);
  nr_cell_bind(0);
  CHECK(wait_written(ca, 3));
  CHECK(wait_written(cb, 5));
  nr_cell_bind(ca);
  CHECK(nr_csirs_disc_worker_ring()->written == 3);
  CHECK(nr_csirs_disc_worker_ring()->n_rb == 24 && nr_csirs_disc_worker_ring()->n_ant == 4);
  nr_cell_bind(cb);
  CHECK(nr_csirs_disc_worker_ring()->written == 5);
  CHECK(nr_csirs_disc_worker_ring()->n_rb == 52 && nr_csirs_disc_worker_ring()->n_ant == 2);
  nr_cell_bind(0);
  CHECK(nr_csirs_disc_worker_ring() == NULL); /* cell 0 was never started */
  CHECK(!nr_csirs_disc_running());

  /* per-cell metrics: each worker ran batches on its own ring only */
  nr_csirs_disc_metrics_t m0 = {0}, ma_m = {0}, mb_m = {0};
  for (int i = 0; i < 300; ++i) {
    nr_csirs_disc_get_metrics_cell(ca, &ma_m);
    nr_csirs_disc_get_metrics_cell(cb, &mb_m);
    if (ma_m.batches && mb_m.batches) break;
    struct timespec d = {0, 10 * 1000 * 1000};
    nanosleep(&d, NULL);
  }
  nr_csirs_disc_get_metrics_cell(0, &m0);
  CHECK(ma_m.batches >= 1 && mb_m.batches >= 1);
  CHECK(m0.batches == 0);
  nr_cell_bind(ca);
  nr_csirs_disc_metrics_t viaget = {0};
  nr_csirs_disc_get_metrics(&viaget);
  CHECK(viaget.batches >= ma_m.batches);
  nr_cell_bind(0);

  nr_csirs_disc_cell_release(ca);
  CHECK(!nr_csirs_disc_cell_mem(ca, &ma));
  CHECK(nr_host_membudget_committed() == mb.ring_host_bytes + mb.td_host_bytes);
  nr_csirs_disc_cell_release(cb);
  nr_csirs_disc_cell_release(cb); /* idempotent */
  ue_free(&ua);
  ue_free(&ub);
}

/* ---------------------------------------------------------------- T4: footprint history arrays (mpci audit)
 * The history arrays are allocated by nr_csirs_disc_fp_prealloc (before the RX fence), kept (zeroed) by nr_csirs_disc_detect_reset and freed by nr_csirs_disc_fp_release;
 * two cells with different carriers hold different, independent arrays. The test runs with the CPU backend (no GPU detect), the path that uses the history. */
static void test_fp_history(void)
{
  nr_cell_bind(9);
  CHECK(nr_csirs_disc_fp_history_bytes() == 0);
  CHECK(nr_csirs_disc_fp_prealloc(0) == -1); /* out of range */
  CHECK(nr_csirs_disc_fp_prealloc(24) == 0);
  const size_t a = nr_csirs_disc_fp_history_bytes();
  CHECK(a > 0);
  nr_cell_bind(10);
  CHECK(nr_csirs_disc_fp_history_bytes() == 0); /* cell 9's arrays are not cell 10's */
  CHECK(nr_csirs_disc_fp_prealloc(106) == 0);
  const size_t b = nr_csirs_disc_fp_history_bytes();
  CHECK(b > a); /* sized from the cell's own N_RB */
  nr_cell_bind(9);
  CHECK(nr_csirs_disc_fp_history_bytes() == a);
  nr_csirs_disc_detect_reset(); /* keeps the arrays: a reset on the discovery worker never allocates */
  CHECK(nr_csirs_disc_fp_history_bytes() == a);
  CHECK(nr_csirs_disc_fp_prealloc(24) == 0 && nr_csirs_disc_fp_history_bytes() == a); /* idempotent */
  nr_cell_bind(10);
  CHECK(nr_csirs_disc_fp_history_bytes() == b);
  nr_csirs_disc_fp_release();
  CHECK(nr_csirs_disc_fp_history_bytes() == 0);
  nr_cell_bind(9);
  CHECK(nr_csirs_disc_fp_history_bytes() == a); /* releasing cell 10 leaves cell 9 alone */
  nr_csirs_disc_fp_release();
  CHECK(nr_csirs_disc_fp_history_bytes() == 0);
  nr_cell_bind(0);
}

int main(void)
{
  setenv("ISAC_CSIRS_DISC_BACKEND", "cpu", 1); /* existing test selector of the CPU reference */
  logInit(); /* before nr_csirs_disc_synth_init(), which installs its own config module */
  load_dftslib();
  InitSinLUT();
  set_glog(OAILOG_ERR);
  test_detector();
  test_td();
  test_prealloc();
  test_fp_history();
  /* after the RX fence an inactive cell answers without touching its (never allocated) heap slot */
  nr_cell_alloc_fence();
  nr_cell_bind(15);
  CHECK(nr_csirs_disc_td_pending() == 0);
  CHECK(nr_csirs_disc_td_push(&(PHY_VARS_NR_UE){0}, 1, 0, 1) == -1);
  nr_csirs_disc_td_reset_cell(15);
  CHECK(nr_csirs_disc_td_drain(NULL) == 0);
  CHECK(!nr_csirs_disc_running());
  nr_cell_bind(0);
  if (failures) {
    fprintf(stderr, "nr_csirs_disc_cell_test: %d failure(s)\n", failures);
    return 1;
  }
  printf("nr_csirs_disc_cell_test: OK\n");
  return 0;
}
