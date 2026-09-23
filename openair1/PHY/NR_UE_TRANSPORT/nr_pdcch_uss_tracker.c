#define _GNU_SOURCE
#include "nr_pdcch_uss_tracker.h"

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "common/utils/LOG/log.h"

#define USS_TRACK_GEOMETRIES 16
#define USS_TRACK_HISTORY 64
#define USS_TRACK_QUEUE 64
#define USS_TRACK_MIN_OBS 8
#define USS_TRACK_MIN_DISTINCT_SLOTS 4
#define USS_TRACK_MEMORY 16
#define USS_TRACK_STABLE_ROUNDS 3

typedef struct {
  uint32_t absolute_slot;
  uint16_t slot;
  uint16_t n_cces;
  nr_pdcch_uss_observation_t o;
} uss_sample_t;

typedef struct {
  bool used;
  nr_pdcch_uss_geometry_t geometry;
  uint64_t touched;
  uint32_t generation;
  uint32_t scored_generation;
  uint32_t score_round;
  uint64_t observations;
  int n;
  int head;
  uss_sample_t sample[USS_TRACK_HISTORY];
  uint16_t top[NR_PDCCH_USS_TRACKER_TOP];
  float top_score[NR_PDCCH_USS_TRACKER_TOP];
  uint16_t top_hits[NR_PDCCH_USS_TRACKER_TOP];
  uint8_t top_hash[NR_PDCCH_USS_TRACKER_TOP];
  uint8_t top_m[NR_PDCCH_USS_TRACKER_TOP];
  struct {
    uint16_t rnti;
    uint16_t hits;
    float score;
    uint32_t last_round;
    uint8_t streak;
  } memory[USS_TRACK_MEMORY];
} uss_geometry_state_t;

typedef struct {
  int state;
  uint32_t generation;
} uss_job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_t g_thread;
static uss_geometry_state_t g_state[USS_TRACK_GEOMETRIES];
static uss_job_t g_queue[USS_TRACK_QUEUE];
static int g_q_head, g_q_tail, g_q_count;
static uint64_t g_touch;
static bool g_enabled = true;

static const uint8_t k_m[7] = {1, 2, 3, 4, 5, 6, 8};
static const uint32_t k_a[3] = {39827, 39829, 39839};
/* allowed[N][M-index][delta] says whether delta=floor(m*N/M) for some candidate m. */
static uint8_t g_allowed[136][7][136];
static uint8_t g_count[136][7];

static bool same_geometry(const nr_pdcch_uss_geometry_t *a, const nr_pdcch_uss_geometry_t *b)
{
  return memcmp(a, b, sizeof(*a)) == 0;
}

static uint32_t mod_pow(uint32_t a, uint32_t e)
{
  uint64_t r = 1;
  while (e) {
    if (e & 1u)
      r = (r * a) % 65537u;
    a = (uint32_t)(((uint64_t)a * a) % 65537u);
    e >>= 1;
  }
  return (uint32_t)r;
}

static int al_index(int al)
{
  switch (al) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 8: return 3;
    case 16: return 4;
    default: return -1;
  }
}

static void insert_top(uint16_t rnti, float score, uint16_t hits, uint8_t hash, uint8_t m,
                       uint16_t out_rnti[NR_PDCCH_USS_TRACKER_TOP],
                       float out_score[NR_PDCCH_USS_TRACKER_TOP],
                       uint16_t out_hits[NR_PDCCH_USS_TRACKER_TOP],
                       uint8_t out_hash[NR_PDCCH_USS_TRACKER_TOP],
                       uint8_t out_m[NR_PDCCH_USS_TRACKER_TOP])
{
  int at = NR_PDCCH_USS_TRACKER_TOP;
  for (int i = 0; i < NR_PDCCH_USS_TRACKER_TOP; ++i)
    if (score > out_score[i]) { at = i; break; }
  if (at == NR_PDCCH_USS_TRACKER_TOP)
    return;
  for (int i = NR_PDCCH_USS_TRACKER_TOP - 1; i > at; --i) {
    out_rnti[i] = out_rnti[i - 1]; out_score[i] = out_score[i - 1];
    out_hits[i] = out_hits[i - 1]; out_hash[i] = out_hash[i - 1]; out_m[i] = out_m[i - 1];
  }
  out_rnti[at] = rnti; out_score[at] = score; out_hits[at] = hits;
  out_hash[at] = hash; out_m[at] = m;
}

static void score_snapshot(int state_index, uint32_t generation,
                           const nr_pdcch_uss_geometry_t *geometry,
                           const uss_sample_t *sample, int n)
{
  uint16_t best_rnti[NR_PDCCH_USS_TRACKER_TOP] = {0};
  float best_score[NR_PDCCH_USS_TRACKER_TOP];
  uint16_t best_hits[NR_PDCCH_USS_TRACKER_TOP] = {0};
  uint8_t best_hash[NR_PDCCH_USS_TRACKER_TOP] = {0}, best_m[NR_PDCCH_USS_TRACKER_TOP] = {0};
  for (int i = 0; i < NR_PDCCH_USS_TRACKER_TOP; ++i)
    best_score[i] = -INFINITY;

  uint32_t multiplier[3][USS_TRACK_HISTORY];
  for (int cid = 0; cid < 3; ++cid)
    for (int j = 0; j < n; ++j)
      multiplier[cid][j] = mod_pow(k_a[cid], (uint32_t)sample[j].slot + 1u);

  /* The full RNTI domain is integer-only here. For a fixed N and M, candidate positions are a
   * translation of seven precomputed delta sets; no polar decode or sample access occurs. */
  for (uint32_t rnti = 1; rnti <= 65535u; ++rnti) {
    float rnti_best = -INFINITY;
    uint16_t rnti_hits = 0;
    uint8_t rnti_hash = 0, rnti_m = 0;
    for (int cid = 0; cid < 3; ++cid) {
      float acc[NR_PDCCH_USS_TRACKER_AL][7] = {{0}};
      uint16_t hits[NR_PDCCH_USS_TRACKER_AL][7] = {{0}};
      bool seen_al[NR_PDCCH_USS_TRACKER_AL] = {false};
      for (int j = 0; j < n; ++j) {
        const int ai = al_index(sample[j].o.al);
        if (ai < 0 || sample[j].o.al == 0 || sample[j].o.cce % sample[j].o.al)
          continue;
        const int N = sample[j].n_cces / sample[j].o.al;
        const int pos = sample[j].o.cce / sample[j].o.al;
        if (N < 2 || N > 135 || pos >= N)
          continue;
        const int y = (int)(((uint64_t)multiplier[cid][j] * rnti % 65537u) % (uint32_t)N);
        const int delta = (pos - y + N) % N;
        const float w = fminf(fmaxf(sample[j].o.score, 0.0f), 1.0f);
        seen_al[ai] = true;
        for (int mi = 0; mi < 7; ++mi) {
          const float p = (float)g_count[N][mi] / (float)N;
          if (p >= 1.0f) continue; /* this M carries no identity information at this N */
          const bool hit = g_allowed[N][mi][delta] != 0;
          acc[ai][mi] += w * ((hit ? 1.0f : 0.0f) - p) / sqrtf(p * (1.0f - p));
          hits[ai][mi] += hit;
        }
      }
      float total = 0.0f;
      uint16_t total_hits = 0;
      uint8_t strongest_m = 0;
      float strongest = -INFINITY;
      for (int ai = 0; ai < NR_PDCCH_USS_TRACKER_AL; ++ai) {
        if (!seen_al[ai]) continue;
        int bmi = 0;
        for (int mi = 1; mi < 7; ++mi)
          if (acc[ai][mi] > acc[ai][bmi]) bmi = mi;
        if (acc[ai][bmi] > 0.0f) total += acc[ai][bmi];
        total_hits += hits[ai][bmi];
        if (acc[ai][bmi] > strongest) { strongest = acc[ai][bmi]; strongest_m = k_m[bmi]; }
      }
      if (total > rnti_best) {
        rnti_best = total; rnti_hits = total_hits; rnti_hash = (uint8_t)cid; rnti_m = strongest_m;
      }
    }
    insert_top((uint16_t)rnti, rnti_best, rnti_hits, rnti_hash, rnti_m,
               best_rnti, best_score, best_hits, best_hash, best_m);
  }

  pthread_mutex_lock(&g_lock);
  uss_geometry_state_t *s = &g_state[state_index];
  if (s->used && same_geometry(&s->geometry, geometry)) {
    memcpy(s->top, best_rnti, sizeof(s->top));
    memcpy(s->top_score, best_score, sizeof(s->top_score));
    memcpy(s->top_hits, best_hits, sizeof(s->top_hits));
    memcpy(s->top_hash, best_hash, sizeof(s->top_hash));
    memcpy(s->top_m, best_m, sizeof(s->top_m));
    s->scored_generation = generation;
    const uint32_t round = ++s->score_round;
    for (int i = 0; i < USS_TRACK_MEMORY; ++i)
      if (s->memory[i].rnti && s->memory[i].last_round + 1 < round)
        s->memory[i].streak = 0;
    for (int k = 0; k < NR_PDCCH_USS_TRACKER_TOP; ++k) {
      int at = -1, victim = 0;
      for (int i = 0; i < USS_TRACK_MEMORY; ++i) {
        if (s->memory[i].rnti == best_rnti[k]) at = i;
        if (!s->memory[i].rnti
            || s->memory[i].last_round < s->memory[victim].last_round
            || (s->memory[i].last_round == s->memory[victim].last_round
                && s->memory[i].streak < s->memory[victim].streak))
          victim = i;
      }
      if (at < 0) {
        at = victim;
        memset(&s->memory[at], 0, sizeof(s->memory[at]));
        s->memory[at].rnti = best_rnti[k];
      }
      s->memory[at].streak = s->memory[at].last_round + 1 == round
                               ? (uint8_t)(s->memory[at].streak + 1) : 1;
      s->memory[at].last_round = round;
      s->memory[at].score = best_score[k];
      s->memory[at].hits = best_hits[k];
    }
    int stable = 0;
    for (int i = 0; i < USS_TRACK_MEMORY; ++i)
      stable += s->observations >= 256 && s->memory[i].rnti
                && s->memory[i].streak >= USS_TRACK_STABLE_ROUNDS;
    LOG_A(PHY, "SENSING: USS_TRACK geometry=rb%u+%u dur%u map=%u/%u/%u id=%u obs=%d "
               "top=0x%04x score=%.3f hits=%u hash=%u M=%u gap=%.3f stable=%d\n",
          geometry->rb_offset, geometry->span_rb, geometry->duration, geometry->bundle,
          geometry->interleaver, geometry->shift, geometry->dmrs_id, n, best_rnti[0],
          best_score[0], best_hits[0], best_hash[0], best_m[0], best_score[0] - best_score[1], stable);
  }
  pthread_mutex_unlock(&g_lock);
}

static void *tracker_worker(void *unused)
{
  (void)unused;
  struct sched_param sp = {0};
  const int sched_rc = pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
  int affinity_rc = 0;
  int core = -1;
  const char *core_env = getenv("ISAC_PDCCH_USS_CORE");
  if (core_env && *core_env) {
    core = atoi(core_env);
    if (core >= 0 && core < CPU_SETSIZE) {
      cpu_set_t set;
      CPU_ZERO(&set);
      CPU_SET(core, &set);
      affinity_rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    } else {
      affinity_rc = EINVAL;
    }
  }
  pthread_setname_np(pthread_self(), "pdcchUssHash");
  LOG_A(PHY, "SENSING: USS_TRACK worker policy=SCHED_OTHER sched_rc=%d core=%d affinity_rc=%d\n",
        sched_rc, core, affinity_rc);
  for (;;) {
    pthread_mutex_lock(&g_lock);
    while (g_q_count == 0)
      pthread_cond_wait(&g_cond, &g_lock);
    const uss_job_t job = g_queue[g_q_head];
    g_q_head = (g_q_head + 1) % USS_TRACK_QUEUE;
    --g_q_count;
    nr_pdcch_uss_geometry_t geometry = {0};
    uint32_t generation = 0;
    uss_sample_t sample[USS_TRACK_HISTORY];
    int n = 0;
    if (job.state >= 0 && job.state < USS_TRACK_GEOMETRIES) {
      const uss_geometry_state_t *s = &g_state[job.state];
      if (s->used) {
        geometry = s->geometry;
        n = s->n;
        generation = s->generation;
        const int first = (s->head - s->n + USS_TRACK_HISTORY) % USS_TRACK_HISTORY;
        for (int i = 0; i < n; ++i)
          sample[i] = s->sample[(first + i) % USS_TRACK_HISTORY];
      }
    }
    pthread_mutex_unlock(&g_lock);
    if (n >= USS_TRACK_MIN_OBS)
      score_snapshot(job.state, generation, &geometry, sample, n);
  }
  return NULL;
}

static void tracker_init(void)
{
  const char *e = getenv("ISAC_PDCCH_USS_TRACKER");
  g_enabled = !e || atoi(e) != 0;
  for (int N = 1; N <= 135; ++N)
    for (int mi = 0; mi < 7; ++mi)
      for (int m = 0; m < k_m[mi]; ++m) {
        const int d = (m * N) / k_m[mi];
        if (!g_allowed[N][mi][d]) {
          g_allowed[N][mi][d] = 1;
          ++g_count[N][mi];
        }
      }
  if (g_enabled && pthread_create(&g_thread, NULL, tracker_worker, NULL) == 0) {
    pthread_detach(g_thread);
    LOG_A(PHY, "SENSING: USS_TRACK enabled: bounded producer, asynchronous 65535-RNTI hash scorer\n");
  } else {
    g_enabled = false;
  }
}

static int find_state(const nr_pdcch_uss_geometry_t *geometry, bool create)
{
  int victim = -1;
  for (int i = 0; i < USS_TRACK_GEOMETRIES; ++i) {
    if (g_state[i].used && same_geometry(&g_state[i].geometry, geometry))
      return i;
    if (!g_state[i].used) victim = i;
    else if (victim < 0 || g_state[i].touched < g_state[victim].touched) victim = i;
  }
  if (!create || victim < 0) return -1;
  memset(&g_state[victim], 0, sizeof(g_state[victim]));
  g_state[victim].used = true;
  g_state[victim].geometry = *geometry;
  return victim;
}

void nr_pdcch_uss_tracker_observe(const nr_pdcch_uss_geometry_t *geometry,
                                  uint32_t absolute_slot, uint16_t slot_in_frame, uint16_t n_cces,
                                  const nr_pdcch_uss_observation_t *observation, int n_observation)
{
  pthread_once(&g_once, tracker_init);
  if (!g_enabled || !geometry || !observation || n_observation <= 0 || n_cces < 2 || n_cces > 135)
    return;
  pthread_mutex_lock(&g_lock);
  const int si = find_state(geometry, true);
  uss_geometry_state_t *s = &g_state[si];
  s->touched = ++g_touch;
  /* Keep only the strongest AL from each occasion. This prevents four noise-only AL maxima from
   * outvoting one real occupied candidate while still allowing the selected AL to vary by grant. */
  int best = 0;
  for (int i = 1; i < n_observation; ++i)
    if (observation[i].score > observation[best].score) best = i;
  const uss_sample_t v = {.absolute_slot = absolute_slot, .slot = slot_in_frame,
                          .n_cces = n_cces, .o = observation[best]};
  bool replaced = false, changed = false;
  for (int i = 0; i < s->n; ++i) {
    const int at = (s->head - 1 - i + USS_TRACK_HISTORY) % USS_TRACK_HISTORY;
    if (s->sample[at].absolute_slot == absolute_slot) {
      if (v.o.score > s->sample[at].o.score) { s->sample[at] = v; changed = true; }
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    s->sample[s->head] = v;
    s->head = (s->head + 1) % USS_TRACK_HISTORY;
    if (s->n < USS_TRACK_HISTORY) ++s->n;
    ++s->observations;
    changed = true;
  }
  if (changed) ++s->generation;
  /* Rescore on distinct-slot milestones. Duplicate geometry passes cannot manufacture recurrence. */
  const bool due = !replaced && (s->observations == 8 || s->observations == 16
                   || s->observations == 32 || s->observations == 64
                   || (s->observations > 64 && (s->observations & 255u) == 0));
  if (due && s->n >= USS_TRACK_MIN_DISTINCT_SLOTS && g_q_count < USS_TRACK_QUEUE) {
    g_queue[g_q_tail] = (uss_job_t){.state = si, .generation = s->generation};
    g_q_tail = (g_q_tail + 1) % USS_TRACK_QUEUE;
    ++g_q_count;
    pthread_cond_signal(&g_cond);
  }
  pthread_mutex_unlock(&g_lock);
}

static int tracker_copy(const nr_pdcch_uss_geometry_t *geometry, uint16_t *rnti,
                        int max_rnti, bool stable_only)
{
  pthread_once(&g_once, tracker_init);
  if (!g_enabled || !geometry || !rnti || max_rnti <= 0) return 0;
  pthread_mutex_lock(&g_lock);
  const int si = find_state(geometry, false);
  int n = 0;
  if (si >= 0 && g_state[si].scored_generation != 0) {
    const uss_geometry_state_t *s = &g_state[si];
    if (!stable_only) {
      for (int i = 0; i < NR_PDCCH_USS_TRACKER_TOP && n < max_rnti; ++i)
        if (s->top[i] && s->top_hits[i] >= USS_TRACK_MIN_DISTINCT_SLOTS)
          rnti[n++] = s->top[i];
    } else {
      bool used[USS_TRACK_MEMORY] = {false};
      while (n < max_rnti) {
        int best = -1;
        for (int i = 0; i < USS_TRACK_MEMORY; ++i)
          if (!used[i] && s->memory[i].rnti
              && s->memory[i].streak >= USS_TRACK_STABLE_ROUNDS
              && s->memory[i].last_round == s->score_round
              && (best < 0 || s->memory[i].score > s->memory[best].score))
            best = i;
        if (best < 0) break;
        used[best] = true;
        rnti[n++] = s->memory[best].rnti;
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

int nr_pdcch_uss_tracker_top(const nr_pdcch_uss_geometry_t *geometry, uint16_t *rnti, int max_rnti)
{
  return tracker_copy(geometry, rnti, max_rnti, true);
}

int nr_pdcch_uss_tracker_peek(const nr_pdcch_uss_geometry_t *geometry, uint16_t *rnti, int max_rnti)
{
  return tracker_copy(geometry, rnti, max_rnti, false);
}
