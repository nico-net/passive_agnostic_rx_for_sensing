/* See nr_ul_llr_dump.h. Env: ISAC_UL_LLR_DUMP=<dir>, _SNR (dB, default 0), _MCS (max, default 16), _MIN_RB (default 10),
 * _MAX (failed grants, default 400), _OK_MAX (CRC-pass controls, default 20), _MAX_MB (byte cap, default 512). */
#include "nr_ul_llr_dump.h"
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NSLOT 4
#define SLOT_CAP (1u << 18) /* int16 LLRs per slot (512 KB); larger grants are dropped */
enum { FREE, FILLING, FILLED };

typedef struct {
  atomic_int st;
  nr_ul_llr_dump_rec_t rec;
  char ss[12];
  int16_t *llr;
} slot_t;

static slot_t g_slot[NSLOT];
static char g_dir[512];
static int g_on = -1;
static float g_snr;
static int g_mcs, g_min_rb;
static long g_max, g_ok_max;
static unsigned long g_max_bytes;
static atomic_long g_n_fail, g_n_ok, g_drop, g_seq;
static atomic_ulong g_bytes;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static long envl(const char *k, long d)
{
  const char *v = getenv(k);
  return v && *v ? atol(v) : d;
}

static void load_env(void)
{
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP not in the frozen config t26zE (2026-10-18); slated for removal */
  const char *d = getenv("ISAC_UL_LLR_DUMP");
  g_on = d && *d ? 1 : 0;
  if (!g_on) return;
  snprintf(g_dir, sizeof(g_dir), "%s", d);
  mkdir(g_dir, 0755); /* EEXIST is fine; a failed open later is reported once per file */
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_SNR not in the frozen config t26zE (2026-10-18); slated for removal */
  g_snr = (float)envl("ISAC_UL_LLR_DUMP_SNR", 0);
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_MCS not in the frozen config t26zE (2026-10-18); slated for removal */
  g_mcs = (int)envl("ISAC_UL_LLR_DUMP_MCS", 16);
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_MIN_RB not in the frozen config t26zE (2026-10-18); slated for removal */
  g_min_rb = (int)envl("ISAC_UL_LLR_DUMP_MIN_RB", 10);
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_MAX not in the frozen config t26zE (2026-10-18); slated for removal */
  g_max = envl("ISAC_UL_LLR_DUMP_MAX", 400);
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_OK_MAX not in the frozen config t26zE (2026-10-18); slated for removal */
  g_ok_max = envl("ISAC_UL_LLR_DUMP_OK_MAX", 20);
  /* DEFERRED/DO NOT USE: ISAC_UL_LLR_DUMP_MAX_MB not in the frozen config t26zE (2026-10-18); slated for removal */
  g_max_bytes = (unsigned long)envl("ISAC_UL_LLR_DUMP_MAX_MB", 512) << 20;
}

bool nr_ul_llr_dump_enabled(void)
{
  if (g_on < 0) load_env(); /* benign race: every thread computes the same values */
  return g_on > 0;
}

static void write_one(slot_t *s)
{
  const nr_ul_llr_dump_rec_t *r = &s->rec;
  char base[640], p[700];
  snprintf(base, sizeof(base), "%s/ul_p%u_s%llu_r%04x_%ld", g_dir, r->pci, (unsigned long long)r->abs_slot, r->rnti,
           atomic_fetch_add(&g_seq, 1));
  snprintf(p, sizeof(p), "%s.llr", base);
  FILE *f = fopen(p, "wb");
  if (!f) { perror(p); return; }
  fwrite(s->llr, sizeof(int16_t), r->G, f);
  fclose(f);
  atomic_fetch_add(&g_bytes, (unsigned long)r->G * 2);
  snprintf(p, sizeof(p), "%s.txt", base); /* the sidecar is written last: its presence means the .llr is complete */
  f = fopen(p, "w");
  if (!f) { perror(p); return; }
  fprintf(f,
          "pci=%u abs_slot=%llu frame=%u slot=%u rnti=0x%04x dci_format=%s search_space=%s cce=%d al=%d start_prb=%u num_rb=%u "
          "start_sym=%u num_sym=%u mcs=%u mcs_table=%u qm=%u R=%u tbs=%u bg=%u rv=%u ndi=%u harq_pid=%u nl=%u tp=%u dmrs_type=%u "
          "dmrs_symb_pos=0x%x cdm_groups=%u dmrs_ports=0x%x nscid=%u dmrs_id=%u tried_id=%u snr_db=%.2f delay=%d ta=%d G=%u "
          "unav_res=%u crc_ok=%d llr_file=%s.llr\n",
          r->pci, (unsigned long long)r->abs_slot, r->frame, r->slot, r->rnti, r->dci_format ? "0_0" : "0_1", r->search_space,
          r->cce, r->al, r->start_prb, r->num_rb, r->start_sym, r->num_sym, r->mcs, r->mcs_table, r->qm, r->R, r->tbs_bits,
          r->bg, r->rv, r->ndi, r->harq_pid, r->nl, r->tp, r->dmrs_type, r->dmrs_symb_pos, r->cdm_groups, r->dmrs_ports,
          r->nscid, r->dmrs_id, r->tried_id, r->snr_db, r->delay, r->ta, r->G, r->unav_res, (int)r->crc_ok,
          strrchr(base, '/') + 1);
  fclose(f);
  fprintf(stderr, "ULLLRDUMP written pci=%u rnti=0x%04x fmt=%s mcs=%u rb=%u snr=%.1f crc_ok=%d fail=%ld ok=%ld drop=%ld\n", r->pci,
          r->rnti, r->dci_format ? "0_0" : "0_1", r->mcs, r->num_rb, r->snr_db, (int)r->crc_ok, (long)atomic_load(&g_n_fail),
          (long)atomic_load(&g_n_ok), (long)atomic_load(&g_drop));
}

static void *writer(void *a)
{
  (void)a;
  nice(19);
  for (;;) {
    bool any = false;
    for (int i = 0; i < NSLOT; i++)
      if (atomic_load(&g_slot[i].st) == FILLED) {
        write_one(&g_slot[i]);
        atomic_store(&g_slot[i].st, FREE);
        any = true;
      }
    if (!any) usleep(20000);
  }
  return NULL;
}

static void start_writer(void)
{
  for (int i = 0; i < NSLOT; i++) g_slot[i].llr = malloc(SLOT_CAP * sizeof(int16_t));
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED); /* never inherit SCHED_FIFO from the PUSCH worker */
  struct sched_param sp = {0};
  pthread_attr_setschedpolicy(&at, SCHED_OTHER);
  pthread_attr_setschedparam(&at, &sp);
  pthread_t t;
  if (pthread_create(&t, &at, writer, NULL) == 0) pthread_detach(t);
  pthread_attr_destroy(&at);
}

int nr_ul_llr_dump_maybe(const nr_ul_llr_dump_rec_t *r, const int16_t *llr)
{
  if (!nr_ul_llr_dump_enabled() || r->probe || r->num_rb < g_min_rb || !isfinite(r->snr_db) || r->snr_db < g_snr || r->mcs > g_mcs
      || r->G == 0 || r->G > SLOT_CAP)
    return 0;
  atomic_long *cnt = r->crc_ok ? &g_n_ok : &g_n_fail;
  if (atomic_load(cnt) >= (r->crc_ok ? g_ok_max : g_max) || atomic_load(&g_bytes) >= g_max_bytes) return 0;
  pthread_once(&g_once, start_writer);
  for (int i = 0; i < NSLOT; i++) {
    int e = FREE;
    if (!g_slot[i].llr || !atomic_compare_exchange_strong(&g_slot[i].st, &e, FILLING)) continue;
    g_slot[i].rec = *r;
    memcpy(g_slot[i].llr, llr, (size_t)r->G * sizeof(int16_t));
    if (!r->search_space || strlen(r->search_space) >= sizeof(g_slot[i].ss)) g_slot[i].rec.search_space = "unknown";
    else { strcpy(g_slot[i].ss, r->search_space); g_slot[i].rec.search_space = g_slot[i].ss; }
    atomic_fetch_add(cnt, 1);
    atomic_store(&g_slot[i].st, FILLED);
    return 1;
  }
  atomic_fetch_add(&g_drop, 1); /* ring full: drop, never wait */
  return 0;
}

void nr_ul_llr_dump_flush(void)
{
  if (g_on <= 0) return;
  for (;;) {
    bool busy = false;
    for (int i = 0; i < NSLOT; i++) busy |= atomic_load(&g_slot[i].st) != FREE;
    if (!busy) return;
    usleep(5000);
  }
}

void nr_ul_llr_dump_reset(void)
{
  nr_ul_llr_dump_flush();
  load_env();
  atomic_store(&g_n_fail, 0); atomic_store(&g_n_ok, 0); atomic_store(&g_drop, 0); atomic_store(&g_bytes, 0);
}
