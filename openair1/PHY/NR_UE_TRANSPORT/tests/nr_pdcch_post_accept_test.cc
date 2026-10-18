/* Unit tests of the A2E ("accept to enqueue") cuts: the post-accept queue, the alive-flag scan and the DCI 1_1 resolver observe pass. */
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>

#include <atomic>
#include <sched.h>
#include <chrono>
#include <pthread.h>
#include <time.h>
#include <algorithm>
extern "C" {
#include "nr_pdcch_blind_monitor.h"
#include "nr_pdcch_post_accept.h"
#include "nr_dci11_observe_golden.h"
}

namespace {

/* ---- the Phase-2 iteration pattern: job first, bookkeeping after, on every exit path ---- */
struct Log {
  std::vector<int> ev; /* +k: job k enqueued; -k: bookkeeping op of iteration k ran */
  std::vector<uint64_t> job_content;
};
Log g_log;
struct OpArgs { int k; uint64_t a, b; };
void op(const void *p)
{
  const OpArgs *o = (const OpArgs *)p;
  g_log.ev.push_back(-o->k);
}
void cleanup(nr_post_accept_t *p) { nr_post_accept_run(p); }

/* reference: the old order (bookkeeping, then job) vs the new one; the job is a pure function of the accepted DCI, never of the bookkeeping */
uint64_t build_job(int k, uint64_t payload) { return payload * 1099511628211ULL ^ (uint64_t)k; }

void iterate(int k, uint64_t payload, int exit_kind /* 0 fall through, 1 continue-like early exit, 2 break-like */, bool deferred)
{
  nr_post_accept_t pa __attribute__((cleanup(cleanup)));
  nr_post_accept_init(&pa);
  const OpArgs a{k, payload, 0};
  if (deferred) {
    nr_post_accept_defer(&pa, op, &a, sizeof(a));
    nr_post_accept_defer(&pa, op, &a, sizeof(a));
  } else {
    op(&a);
    op(&a);
  }
  if (exit_kind == 1) {
    const uint64_t j = build_job(k, payload);
    g_log.ev.push_back(k);
    g_log.job_content.push_back(j);
    return; /* an early exit between the enqueue and the end of the iteration */
  }
  const uint64_t j = build_job(k, payload);
  g_log.ev.push_back(k);
  g_log.job_content.push_back(j);
  if (exit_kind == 2) return;
}

TEST(PostAccept, JobEnqueuedBeforeBookkeepingIdenticalContent)
{
  for (int kind = 0; kind < 3; kind++) {
    g_log = Log();
    iterate(7, 0xabcdefULL, kind, true);
    const Log nw = g_log;
    g_log = Log();
    iterate(7, 0xabcdefULL, kind, false);
    const Log old = g_log;
    ASSERT_EQ(nw.ev.size(), 3u);
    EXPECT_EQ(nw.ev[0], 7) << "the job must be enqueued first";
    EXPECT_EQ(nw.ev[1], -7);
    EXPECT_EQ(nw.ev[2], -7);
    ASSERT_EQ(old.ev.size(), 3u);
    EXPECT_EQ(old.ev[2], 7) << "reference order: bookkeeping first";
    EXPECT_EQ(nw.job_content, old.job_content) << "same job content in both orders";
  }
}

TEST(PostAccept, FifoOnceAndFullQueueRunsAtOnce)
{
  g_log = Log();
  {
    nr_post_accept_t pa __attribute__((cleanup(cleanup)));
    nr_post_accept_init(&pa);
    for (int k = 1; k <= NR_POST_ACCEPT_MAX_OPS + 3; k++) {
      const OpArgs a{k, 0, 0};
      nr_post_accept_defer(&pa, op, &a, sizeof(a));
    }
    /* the three that did not fit ran immediately (the old behaviour), the queued ones wait for the end of the scope */
    ASSERT_EQ(g_log.ev.size(), 3u);
    EXPECT_EQ(g_log.ev[0], -(NR_POST_ACCEPT_MAX_OPS + 1));
  }
  ASSERT_EQ(g_log.ev.size(), (size_t)NR_POST_ACCEPT_MAX_OPS + 3);
  for (int k = 1; k <= NR_POST_ACCEPT_MAX_OPS; k++) EXPECT_EQ(g_log.ev[3 + k - 1], -k);
  /* running twice runs nothing twice */
  nr_post_accept_t pa;
  nr_post_accept_init(&pa);
  const OpArgs a{99, 0, 0};
  nr_post_accept_defer(&pa, op, &a, sizeof(a));
  nr_post_accept_run(&pa);
  nr_post_accept_run(&pa);
  EXPECT_EQ(std::count(g_log.ev.begin(), g_log.ev.end(), -99), 1);
}

/* ---- nr_alive_next == the plain scan, every length and start ---- */
TEST(AliveNext, MatchesLinearScan)
{
  uint64_t s = 99;
  for (int n = 0; n < 70; n++)
    for (int trial = 0; trial < 40; trial++) {
      std::vector<char> v(n + 16, 0);
      bool *alive = (bool *)v.data() + 3; /* unaligned on purpose */
      const int density = trial % 5; /* 0 empty .. 4 dense */
      for (int i = 0; i < n; i++) alive[i] = density && (g11_rng(&s) % (1 + 6 * (5 - density)) == 0);
      for (int i = 0; i <= n; i++) {
        int ref = i;
        while (ref < n && !alive[ref]) ref++;
        ASSERT_EQ(nr_alive_next(alive, i, n), ref) << "n=" << n << " i=" << i;
      }
    }
}

/* ---- the resolver observe pass: state hash equals the one of the implementation before the alive-scan / single-eval cut ---- */
TEST(Dci11ObserveGolden, StateHashUnchanged)
{
  /* golden values produced by the unmodified nr_pdcch_dci11_layout_sweep.c (git 310a86d899) with the same harness */
  EXPECT_EQ(g11_observe_run(106, 6000, nullptr, 0, nullptr), 0xb162c1811a7999fdULL);
  EXPECT_EQ(g11_observe_run(24, 6000, nullptr, 0, nullptr), 0xabc00b0fec068ba4ULL);
}

TEST(Dci11ObserveGolden, BenchObserveRntiCost)
{
  char info[200];
  double us = 0;
  g11_observe_run(106, 6000, info, sizeof info, &us);
  printf("[BENCH] nr_dci11_resolver_observe_rnti %s: %.2f us per accepted DCI\n", info, us);
  EXPECT_GT(us, 0.0);
}

/* ---- the solver reading pass of run_occasion_one() (group live layouts by offset key, parse one per key): old scan vs alive-only scan ---- */
bool parse_full(const nr_dci11_resolver_t *r, int i, const nr_pdcch_blind_raw_result_t *raw, uint16_t len, int bwp_size, int bwp_start,
                int typeA, nr_pdcch_blind_result_t *out)
{
  nr_dci11_field_bits_t f;
  if (!nr_dci11_layout_to_field_bits(&r->hyp[i], &f)) return false;
  nr_pdcch_blind_extract_opts_t o = {};
  o.bwp_indicator_bits = f.bwp_indicator_bits; o.vrb_to_prb_bits = f.vrb_to_prb_bits; o.prb_bundling_bits = f.prb_bundling_bits;
  o.rate_matching_bits = f.rate_matching_bits; o.zp_csirs_bits = f.zp_csirs_bits; o.tb2_bits = f.tb2_bits; o.harq_pid_bits = f.harq_pid_bits;
  o.dai_bits = f.dai_bits; o.pdsch_to_harq_bits = f.pdsch_to_harq_bits; o.antenna_ports_bits = f.antenna_ports_bits;
  o.dmrs_config_type = f.dmrs_config_type; o.tci_bits = f.tci_bits; o.srs_request_bits = f.srs_request_bits; o.cbg_bits = f.cbg_bits;
  o.fdra_mode = f.fdra_mode; o.fdra_bwp_start = bwp_start;
  const uint8_t htb = r->off[i].tda_bits;
  o.tda_count = htb == 0 ? 1 : (1 << htb);
  o.dmrs_add_pos = 0; o.dmrs_max_length = 1;
  for (int k = 0; k < o.tda_count && k < 16; ++k) { o.tda_start[k] = 1; o.tda_length[k] = 13; o.tda_mapping[k] = 0; }
  if (nr_pdcch_blind_dci_size_ex(bwp_size, &o) != len) return false;
  return nr_pdcch_blind_extract_11(raw, len, bwp_size, typeA, &o, out);
}
struct Pass { int nrd = 0, nparse = 0; uint64_t h = 1469598103934665603ULL; };
/* the grouping loop of the monitor, verbatim in structure; alive_only selects the new scan */
Pass reading_pass(const nr_dci11_resolver_t *R, const nr_pdcch_blind_raw_result_t *raw, uint16_t dlen, int bwp, bool alive_only, int rep,
                  uint8_t *s_gid)
{
  enum { KEYS = 256, READINGS = 128 };
  Pass P;
  const int nh = R->n_hyp;
  uint64_t kkey[KEYS]; int kgrp[KEYS], nk = 0;
  static nr_pdcch_blind_result_t rparsed[READINGS];
  int rcount[READINGS] = {0};
  if (alive_only) memset(s_gid, 0xff, (size_t)nh);
  for (int ii = 0; ii < nh; ii++) {
    if (alive_only) {
      int nx = nr_alive_next(R->alive, ii, nh);
      if (rep >= ii && rep < nx) nx = rep;
      if (nx >= nh) break;
      ii = nx;
    }
    const int i = ii;
    s_gid[i] = 0xff;
    if (!R->alive[i] && i != rep) continue;
    const nr_dci11_offsets_t *of = &R->off[i];
    const unsigned ap = nr_dci_bits_field(&raw->payload, of->total, of->ant_ports, of->ant_ports_bits);
    if ((of->ant_ports_bits == 5 && !R->hyp[i].dmrs_type && ap >= 12) || (of->ant_ports_bits == 6 && R->hyp[i].dmrs_type && ap >= 24)) continue;
    const uint64_t key = (uint64_t)of->riv | (uint64_t)of->mcs << 8 | (uint64_t)of->ant_ports << 16 | (uint64_t)of->ant_ports_bits << 24
                         | (uint64_t)of->fdra_mode << 28 | (uint64_t)of->n_rbg << 32 | (uint64_t)R->hyp[i].dmrs_type << 40
                         | (uint64_t)of->tda << 44 | (uint64_t)of->tda_bits << 52;
    int kk = -1;
    for (int q = 0; q < nk && kk < 0; q++) if (kkey[q] == key) kk = q;
    if (kk >= 0) {
      if (kgrp[kk] >= 0) { s_gid[i] = (uint8_t)kgrp[kk]; rcount[kgrp[kk]]++; }
      continue;
    }
    if (nk < KEYS) { kkey[nk] = key; kgrp[nk] = -1; kk = nk++; }
    nr_pdcch_blind_result_t pr;
    P.nparse++;
    if (!parse_full(R, i, raw, dlen, bwp, 0, 2, &pr) || pr.vrb_to_prb) continue;
    int g = -1;
    for (int k = 0; k < P.nrd && g < 0; k++)
      if (rparsed[k].start_rb == pr.start_rb && rparsed[k].num_rb == pr.num_rb && rparsed[k].mcs == pr.mcs && rparsed[k].dmrs_ports == pr.dmrs_ports) g = k;
    if (g < 0 && P.nrd < READINGS) { g = P.nrd++; rparsed[g] = pr; }
    if (g >= 0) { s_gid[i] = (uint8_t)g; rcount[g]++; }
    if (kk >= 0) kgrp[kk] = g;
  }
  /* ids of each reading, ascending (the second loop of the monitor) */
  for (int ii = 0; ii < nh; ii++) {
    if (alive_only) {
      int nx = nr_alive_next(R->alive, ii, nh);
      if (rep >= ii && rep < nx) nx = rep;
      if (nx >= nh) break;
      ii = nx;
    }
    if (s_gid[ii] != 0xff) { P.h = g11_fnv(P.h, &ii, sizeof(ii)); P.h = g11_fnv(P.h, &s_gid[ii], 1); }
  }
  P.h = g11_fnv(P.h, rcount, sizeof(rcount));
  return P;
}

TEST(Dci11ObserveGolden, ReadingPassAliveScanIsIdenticalAndFaster)
{
  int len = 0;
  nr_dci11_resolver_t *R = g11_observe_state(106, 3000, &len, nullptr);
  uint64_t rng = 5;
  std::vector<uint8_t> g_old(NR_DCI11_LAYOUT_MAX), g_new(NR_DCI11_LAYOUT_MAX);
  double t_old = 0, t_new = 0;
  int n = 0, parses = 0;
  for (int rep = -1; rep < 40; rep++) {
    nr_pdcch_blind_raw_result_t raw = {};
    raw.payload = {{g11_rng(&rng), g11_rng(&rng), g11_rng(&rng)}};
    const nr_dci11_offsets_t *o = &R->off[R->n_hyp / 2];
    g11_poke(&raw.payload, o->total, 0, 1, 1);
    g11_poke(&raw.payload, o->total, o->mcs, 5, g11_rng(&rng) % 20);
    g11_poke(&raw.payload, o->total, o->riv, (uint8_t)(o->tda - o->riv), 1 + g11_rng(&rng) % 900);
    const int rp = rep < 0 ? -1 : (rep % 2 ? R->n_hyp / 2 : (int)(g11_rng(&rng) % R->n_hyp)); /* representative: alive, dead or none */
    auto a = std::chrono::steady_clock::now();
    const Pass po = reading_pass(R, &raw, (uint16_t)len, 106, false, rp, g_old.data());
    auto b = std::chrono::steady_clock::now();
    const Pass pn = reading_pass(R, &raw, (uint16_t)len, 106, true, rp, g_new.data());
    auto c = std::chrono::steady_clock::now();
    ASSERT_EQ(po.nrd, pn.nrd);
    ASSERT_EQ(po.nparse, pn.nparse);
    ASSERT_EQ(po.h, pn.h) << "readings / ids differ, rep=" << rp;
    if (rep >= 0) {
      t_old += std::chrono::duration<double, std::micro>(b - a).count();
      t_new += std::chrono::duration<double, std::micro>(c - b).count();
      n++;
      parses += po.nparse;
    }
  }
  printf("[BENCH] reading pass (n_hyp=%d alive=%d, %d parses/DCI): old scan %.1f us, alive-only scan %.1f us per DCI\n", R->n_hyp, R->n_alive,
         parses / n, t_old / n, t_new / n);
  free(R);
}

/* ---- scan-queue hand-off: producer enqueues under the lock and signals, consumer sleeps on the condvar (the shape of nr_pdcch_passive_queue.c) ---- */
struct Hand {
  pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
  int count = 0, stop = 0;
  uint64_t t_enq = 0;
  std::vector<double> lat;
  bool signal_after_unlock = false;
  int spin_us = 0;
  std::atomic<int> hint{0};
};
uint64_t mono_ns()
{
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void *consumer(void *a)
{
  Hand *h = (Hand *)a;
  for (;;) {
    if (h->spin_us > 0) { /* bounded wait for the next job without sleeping: the consumer's core never enters idle */
      const uint64_t end = mono_ns() + (uint64_t)h->spin_us * 1000;
      while (h->hint.load(std::memory_order_acquire) == 0 && mono_ns() < end) sched_yield();
    }
    pthread_mutex_lock(&h->mu);
    while (h->count == 0 && !h->stop) pthread_cond_wait(&h->cv, &h->mu);
    if (h->count == 0) { pthread_mutex_unlock(&h->mu); break; }
    h->count--;
    h->hint.store(h->count, std::memory_order_release);
    pthread_mutex_unlock(&h->mu);
    h->lat.push_back((double)(mono_ns() - h->t_enq) / 1e3); /* enqueue (lock taken) -> consumer running with the job */
  }
  return nullptr;
}
double handoff_p50(bool after_unlock, std::vector<double> *all, int spin_us = 0)
{
  Hand h;
  h.signal_after_unlock = after_unlock;
  h.spin_us = spin_us;
  pthread_t th;
  pthread_create(&th, nullptr, consumer, &h);
  for (int i = 0; i < 1500; i++) {
    timespec ts = {0, 500000}; /* a slot */
    nanosleep(&ts, nullptr);
    pthread_mutex_lock(&h.mu);
    h.t_enq = mono_ns();
    h.count++;
    h.hint.store(h.count, std::memory_order_release);
    if (!after_unlock) pthread_cond_signal(&h.cv);
    /* a few us of work the producer still does under the lock (ring copy, epoch stamp) */
    for (volatile int k = 0; k < 200; k++) {}
    pthread_mutex_unlock(&h.mu);
    if (after_unlock) pthread_cond_signal(&h.cv);
  }
  pthread_mutex_lock(&h.mu);
  h.stop = 1;
  pthread_cond_broadcast(&h.cv);
  pthread_mutex_unlock(&h.mu);
  pthread_join(th, nullptr);
  std::vector<double> v = h.lat;
  std::sort(v.begin(), v.end());
  if (all) *all = v;
  return v.empty() ? 0 : v[v.size() / 2];
}
TEST(ScanQueueHandoff, BenchSignalUnderLockVsAfterUnlock)
{
  std::vector<double> a, b, c;
  const double p_old = handoff_p50(false, &a), p_new = handoff_p50(true, &b), p_spin = handoff_p50(true, &c, 300);
  printf("[BENCH] same, consumer spins (sched_yield) up to 300 us before sleeping: p50 %.1f p99 %.1f us\n", p_spin, c.empty() ? 0.0 : c[c.size() * 99 / 100]);
  const double p99_old = a.empty() ? 0 : a[a.size() * 99 / 100], p99_new = b.empty() ? 0 : b[b.size() * 99 / 100];
  printf("[BENCH] scan-queue hand-off enqueue->consumer running (idle consumer, 500 us period, 1500 each): signal under lock p50 %.1f p99 %.1f us, "
         "after unlock p50 %.1f p99 %.1f us\n", p_old, p99_old, p_new, p99_new);
  EXPECT_GT(a.size(), 1000u);
  EXPECT_GT(b.size(), 1000u);
}

} // namespace
