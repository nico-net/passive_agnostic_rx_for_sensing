/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* PRACH is a receiver observation: use the broadcast RACH geometry and the
 * existing gNB correlator, then export only detections. No receiver state is
 * changed by an overheard RAR. */
#include "nr_prach_passive.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "PHY/defs_gNB.h"
#include "PHY/NR_TRANSPORT/nr_transport_common_proto.h"
#include "PHY/NR_TRANSPORT/nr_transport_proto.h"
#include "PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.h"
#include "PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h"
#include "PHY/NR_UE_TRANSPORT/nr_sense_api.h"
#include "PHY/NR_UE_ISAC/nr_isac.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"

#define PRACH_PENDING 64
#define PRACH_RAR_RING 64
#define PRACH_RAR_WAIT_SLOTS 160
#define PRACH_MAX_RX 4
#define PRACH_PREFIX 4096
#define PRACH_WINDOW 1100000

typedef struct {
  bool valid;
  uint64_t abs_slot;
  uint64_t mono_ns;
  uint32_t epoch, identity_gen, dedicated_epoch;
  int frame, slot, ra_slot, slots_per_frame, fdm, symbol, preamble, energy, delay_samples, pci;
} prach_detection_t;

static prach_detection_t pending[PRACH_PENDING]; /* only the slot producer touches this */
static unsigned pending_head;
/* RAR worker publishes one packed, complete observation per cell. The low 24
 * slot bits cover an SFN cycle; the join compares modulo 1024 frames. */
static _Atomic uint64_t rar_events[PRACH_RAR_RING];
static _Atomic unsigned rar_head;
static atomic_flag prach_busy = ATOMIC_FLAG_INIT;
static c16_t prach_xu[64][839];
static c16_t prach_freq[PRACH_MAX_RX][NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX][NR_PRACH_SEQ_LEN_L];
static c16_t prev_tail[PRACH_MAX_RX][PRACH_PREFIX];
static c16_t prach_window[PRACH_MAX_RX][PRACH_WINDOW];
static uint64_t prev_tail_slot = UINT64_MAX;
static int cached_root = -1, cached_count, cached_short;
static int noise_db10 = -1;

/* The gNB scheduler owns this helper in normal PHY_NR builds. The passive UE
 * links only the receive half of nr_prach.c, so provide its identical table. */
int get_nr_prach_duration(uint8_t format)
{
  static const int duration[] = {0, 0, 0, 0, 2, 4, 6, 2, 12, 2, 6, 2, 4, 6};
  return format < sizeof(duration) / sizeof(duration[0]) ? duration[format] : 0;
}

static int fapi_format(unsigned fmt)
{
  switch (fmt) {
    case 0: case 1: case 2: case 3: return fmt;
    case 0xa1: return 4;
    case 0xa2: return 5;
    case 0xa3: return 6;
    case 0xb1: return 7;
    case 0xb4: return 8;
    case 0xc0: return 9;
    case 0xc2: return 10;
    default: return -1;
  }
}

bool nr_prach_passive_occasion(int config_index, int frame, int slot, int mu, frequency_range_t range,
                               nr_prach_passive_occasion_t *out)
{
  if (!out || config_index < 0 || config_index > 255 || frame < 0 || slot < 0 || mu < 0 || mu > 4
      || (range == FR2 && mu < 2)
      || slot >= (10 << mu))
    return false;
  nr_prach_info_t info = get_nr_prach_occasion_info_from_index(config_index, range, TDD);
  uint16_t index = 0;
  if (!info.x || !get_nr_prach_sched_from_info(info, config_index, frame, slot, mu, range, &index, TDD))
    return false;
  const int fmt = fapi_format(info.format & 0xff);
  if (fmt < 0 || info.N_t_slot == 0 || info.N_t_slot > NUMBER_OF_NR_RU_PRACH_OCCASIONS_MAX)
    return false;
  *out = (nr_prach_passive_occasion_t){fmt, (int)info.start_symbol, (int)info.N_t_slot, (int)info.N_dur};
  return true;
}

int nr_prach_passive_rar_relation(int frame, int slot, int start_symbol, int fdm,
                                  int preamble, uint16_t ra_rnti, uint8_t rapid)
{
  (void)frame;
  if (slot < 0 || slot > 79 || start_symbol < 0 || start_symbol > 13 || fdm < 0 || fdm > 7
      || preamble < 0 || preamble > 63)
    return -1;
  const int expected = 1 + start_symbol + 14 * slot + 1120 * fdm;
  return ra_rnti != expected ? -1 : (preamble == rapid ? 1 : 0);
}

static uint32_t rar_slot_age(uint64_t detection_abs, uint32_t rar_abs, int slots_per_frame)
{
  /* The PDSCH/RAR path provides frame*slots_per_frame+slot, whereas the
   * PRACH producer includes HFN. Compare within the 1024-frame SFN cycle. */
  const uint32_t wrap = 1024u * (uint32_t)slots_per_frame;
  return ((rar_abs % wrap) + wrap - (detection_abs % wrap)) % wrap;
}

static int ra_slot(int slot, int mu, int format)
{
  /* 38.211 long preambles use the subframe index for RA-RNTI t_id. */
  return format < 4 ? slot >> mu : slot;
}

void nr_prach_passive_rar(uint16_t ra_rnti, uint8_t rapid, uint16_t ta, uint64_t abs_slot)
{
  if (!nr_isac_prach_enabled()) return;
  const uint64_t packed = ((abs_slot & 0xffffffULL) << 40) | ((uint64_t)ra_rnti << 24)
                          | ((uint64_t)rapid << 16) | ta;
  const unsigned pos = atomic_fetch_add_explicit(&rar_head, 1, memory_order_relaxed);
  atomic_store_explicit(&rar_events[pos % PRACH_RAR_RING], packed, memory_order_release);
}

int nr_prach_passive_rar_entries(const uint8_t *tb, uint32_t len, nr_prach_rar_entry_t *out, int capacity)
{
  if (!tb || !out || capacity <= 0) return 0;
  int count = 0;
  for (uint32_t i = 0; i < len;) {
    const uint8_t sub = tb[i];
    const bool more = (sub & 0x80) != 0;
    if (sub & 0x40) {
      if (i + 8 > len) break; /* validated parser already checked this */
      const uint8_t *q = tb + i + 1;
      const uint16_t ta = ((q[0] & 0x7f) << 5) | ((q[1] >> 3) & 0x1f);
      out[count++] = (nr_prach_rar_entry_t){sub & 0x3f, ta};
      if (count == capacity) break;
      i += 8;
    } else {
      i++;
    }
    if (!more) break;
  }
  return count;
}

void nr_prach_passive_rar_pdu(uint16_t ra_rnti, uint64_t abs_slot, const uint8_t *tb, uint32_t len)
{
  if (!nr_isac_prach_enabled()) return;
  nr_prach_rar_entry_t entries[64];
  const int count = nr_prach_passive_rar_entries(tb, len, entries, 64);
  for (int i = 0; i < count; ++i)
    nr_prach_passive_rar(ra_rnti, entries[i].rapid, entries[i].ta, abs_slot);
}

static void format_record(const prach_detection_t *d, int relation, int rapid, int ta,
                          char *line, size_t capacity)
{
  char rar_match[8], rar_rapid[16], rar_ta[16], epoch[16], identity[16], dedicated[16];
  snprintf(rar_match, sizeof(rar_match), "%s", relation < 0 ? "null" : (relation ? "true" : "false"));
  snprintf(rar_rapid, sizeof(rar_rapid), rapid < 0 ? "null" : "%d", rapid);
  snprintf(rar_ta, sizeof(rar_ta), ta < 0 ? "null" : "%d", ta);
  snprintf(epoch, sizeof(epoch), d->epoch == UINT32_MAX ? "null" : "%u", d->epoch);
  snprintf(identity, sizeof(identity), d->epoch == UINT32_MAX ? "null" : "%u", d->identity_gen);
  snprintf(dedicated, sizeof(dedicated), d->epoch == UINT32_MAX ? "null" : "%u", d->dedicated_epoch);
  snprintf(line, capacity,
           "{\"schema\":2,\"type\":\"rs\",\"source\":\"prach\",\"dir\":\"UL\","
           "\"epoch\":%s,\"identity_gen\":%s,\"dedicated_epoch\":%s,\"abs_slot\":%llu,"
           "\"frame\":%d,\"slot\":%d,\"t_mono_ns\":%llu,\"rx_ts\":null,\"pci\":%d,"
           "\"rnti\":null,\"start_rb\":null,\"nb_rb\":null,\"n_re\":null,\"l_syms\":[],"
           "\"ports\":null,\"n_ant\":null,\"noise_var\":null,\"slot_frac\":null,"
           "\"cfr_frame_id\":null,\"preamble\":%d,\"energy\":%d,\"delay_samples\":%d,"
           "\"occasion\":{\"frame\":%d,\"slot\":%d,\"fdm_index\":%d,\"symbol\":%d},"
           "\"rar_match\":%s,\"rar_rapid\":%s,\"rar_ta\":%s}",
           epoch, identity, dedicated, (unsigned long long)d->abs_slot, d->frame, d->slot,
           (unsigned long long)d->mono_ns, d->pci, d->preamble, d->energy, d->delay_samples,
           d->frame, d->slot, d->fdm, d->symbol, rar_match, rar_rapid, rar_ta);
}

static void emit_detection(const prach_detection_t *d, uint64_t now)
{
  int relation = -1, rapid = -1, ta = -1;
  const unsigned count = atomic_load_explicit(&rar_head, memory_order_acquire);
  for (unsigned i = 0; i < PRACH_RAR_RING && i < count; ++i) {
    const uint64_t v = atomic_load_explicit(&rar_events[i], memory_order_acquire);
    const uint32_t rar_slot = (v >> 40) & 0xffffff;
    const uint32_t age = rar_slot_age(d->abs_slot, rar_slot, d->slots_per_frame);
    if (age > PRACH_RAR_WAIT_SLOTS || age > now - d->abs_slot) continue;
    const int r = nr_prach_passive_rar_relation(d->frame, d->ra_slot, d->symbol, d->fdm,
                                               d->preamble, (v >> 24) & 0xffff, (v >> 16) & 0xff);
    if (r >= 0 && r > relation) {
      relation = r;
      rapid = (v >> 16) & 0xff;
      ta = v & 0xffff;
    }
  }
  char line[768];
  format_record(d, relation, rapid, ta, line, sizeof(line));
  nr_sense_push_record(line);
}

#ifdef NR_PASSIVE_PRACH_TEST
int nr_prach_passive_rar_age_test(uint64_t detection_abs, uint64_t rar_abs, int slots_per_frame)
{
  return rar_slot_age(detection_abs, rar_abs, slots_per_frame);
}
int nr_prach_passive_ra_slot_test(int slot, int mu, int format)
{
  return ra_slot(slot, mu, format);
}
void nr_prach_passive_format_test(int relation, char *line, uint32_t capacity)
{
  const prach_detection_t d = {.valid = true, .abs_slot = 1234, .mono_ns = 987654321,
                               .epoch = 4, .identity_gen = 2, .dedicated_epoch = 3,
                               .frame = 61, .slot = 14, .ra_slot = 14, .slots_per_frame = 20,
                               .fdm = 1, .symbol = 2,
                               .preamble = 17, .energy = 420, .delay_samples = 32, .pci = 42};
  format_record(&d, relation, relation < 0 ? -1 : 17, relation < 0 ? -1 : 20, line, capacity);
}
#endif

static void flush_pending(uint64_t now)
{
  for (int i = 0; i < PRACH_PENDING; ++i) {
    prach_detection_t *d = &pending[i];
    if (!d->valid) continue;
    /* A matching RAR is decisive immediately; otherwise hold the record long
     * enough to distinguish a different RAPID from a missing RAR. */
    bool matched = false;
    const unsigned count = atomic_load_explicit(&rar_head, memory_order_acquire);
    for (unsigned j = 0; j < PRACH_RAR_RING && j < count; ++j) {
      const uint64_t v = atomic_load_explicit(&rar_events[j], memory_order_acquire);
      const uint32_t age = rar_slot_age(d->abs_slot, (v >> 40) & 0xffffff, d->slots_per_frame);
      if (age > PRACH_RAR_WAIT_SLOTS || age > now - d->abs_slot) continue;
      if (nr_prach_passive_rar_relation(d->frame, d->ra_slot, d->symbol, d->fdm,
                                       d->preamble, (v >> 24) & 0xffff, (v >> 16) & 0xff) == 1) {
        matched = true;
        break;
      }
    }
    if (matched || now - d->abs_slot >= PRACH_RAR_WAIT_SLOTS) {
      emit_detection(d, now);
      d->valid = false;
    }
  }
}

void nr_prach_passive_flush(void)
{
  if (!nr_isac_prach_enabled()) return;
  for (int i = 0; i < PRACH_PENDING; ++i) {
    if (pending[i].valid) {
      emit_detection(&pending[i], pending[i].abs_slot + PRACH_RAR_WAIT_SLOTS);
      pending[i].valid = false;
    }
  }
}

static void process_slot(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_isac_prach_enabled() || !ue || !proc) return;
  const NR_DL_FRAME_PARMS *fp = &ue->frame_parms;
  const uint64_t now = ((uint64_t)proc->hfn_rx * 1024 + proc->frame_rx) * fp->slots_per_frame + proc->nr_slot_rx;
  flush_pending(now);
  if (proc->nr_slot_rx == fp->slots_per_frame - 1 && fp->samples_per_frame >= PRACH_PREFIX) {
    for (int a = 0; a < fp->nb_antennas_rx && a < PRACH_MAX_RX; ++a)
      memcpy(prev_tail[a], ue->common_vars.rxdata[a] + fp->samples_per_frame - PRACH_PREFIX,
             sizeof(prev_tail[a]));
    prev_tail_slot = now;
  }
  const nr_pdcch_sib1_prior_t *prior = nr_pdcch_sib1_prior_get();
  if (!prior || !prior->rach_valid || fp->frame_type != TDD || proc->rx_slot_type == NR_DOWNLINK_SLOT) {
    static bool warned;
    if (!warned && (!prior || !prior->rach_valid)) {
      warned = true;
      LOG_I(PHY, "SENSING: passive PRACH inactive until SIB1 RACH config is available\n");
    }
    return;
  }
  const fapi_nr_prach_config_t *cfg = &ue->nrUE_config.prach_config;
  if (!cfg->num_prach_fd_occasions_list || !cfg->num_prach_fd_occasions) return;
  /* Long PRACH can extend across slots: process only when its final slot has
   * arrived, while keeping p.slot at the original occasion start. */
  const nr_prach_info_t table = get_nr_prach_occasion_info_from_index(prior->prach_config_index, fp->freq_range, TDD);
  const int format = fapi_format(table.format & 0xff);
  if (format < 0) return;
  const int span = format < 4 ? get_long_prach_dur(format, fp->numerology_index) : 1;
  const int start_slot = proc->nr_slot_rx - span + 1;
  nr_prach_passive_occasion_t o;
  if (start_slot < 0 || !nr_prach_passive_occasion(prior->prach_config_index, proc->frame_rx, start_slot,
                                                   fp->numerology_index, fp->freq_range, &o)) return;
  const int n_rx = fp->nb_antennas_rx < PRACH_MAX_RX ? fp->nb_antennas_rx : PRACH_MAX_RX;
  if (n_rx <= 0) return;
  int32_t *time_rx[PRACH_MAX_RX];
  for (int a = 0; a < n_rx; ++a) time_rx[a] = (int32_t *)ue->common_vars.rxdata[a];
  if (start_slot == 0) {
    /* N_TA_offset can place the DFT before sample zero. Stitch the prior
     * frame tail to the samples already filled by the RF reader. */
    const int filled = get_samples_slot_timestamp(fp, proc->nr_slot_rx)
                       + get_samples_per_slot(proc->nr_slot_rx, fp) + fp->ofdm_symbol_size;
    if (prev_tail_slot == UINT64_MAX || prev_tail_slot + 1 + proc->nr_slot_rx != now
        || filled + PRACH_PREFIX > PRACH_WINDOW) return;
    for (int a = 0; a < n_rx; ++a) {
      memcpy(prach_window[a], prev_tail[a], sizeof(prev_tail[a]));
      memcpy(prach_window[a] + PRACH_PREFIX, ue->common_vars.rxdata[a], (size_t)filled * sizeof(c16_t));
      time_rx[a] = (int32_t *)(prach_window[a] + PRACH_PREFIX);
    }
  }
  for (int fdm = 0; fdm < cfg->num_prach_fd_occasions; ++fdm) {
    const fapi_nr_num_prach_fd_occasions_t *fd = &cfg->num_prach_fd_occasions_list[fdm];
    if (fd->num_root_sequences == 0 || fd->num_root_sequences > 64) continue;
    if (cached_root != fd->prach_root_sequence_index || cached_count != fd->num_root_sequences
        || cached_short != cfg->prach_sequence_length) {
      compute_nr_prach_seq(cfg->prach_sequence_length, fd->num_root_sequences,
                           fd->prach_root_sequence_index, prach_xu);
      cached_root = fd->prach_root_sequence_index;
      cached_count = fd->num_root_sequences;
      cached_short = cfg->prach_sequence_length;
    }
    memset(prach_freq, 0, sizeof(prach_freq));
    time_stats_t timing = {0};
    prach_item_t item = {
        .frame = proc->frame_rx, .slot = start_slot, .num_slots = 1,
        .rootSequenceIndex = fd->prach_root_sequence_index,
        .numrootSequenceIndex = fd->num_root_sequences, .msg1_frequencystart = fd->k1,
        .mu = cfg->prach_sub_c_spacing, .prach_sequence_length = cfg->prach_sequence_length,
        .restricted_set = cfg->restricted_set_config, .numerology_index = fp->numerology_index,
        .nb_rx = n_rx, .Xu = prach_xu, .rx_prach = &timing, .prach_buf = prach_freq};
    item.pdu.prach_format = o.format;
    item.pdu.prach_start_symbol = o.start_symbol;
    item.pdu.num_prach_ocas = o.num_occasions;
    item.pdu.num_ra = fdm;
    item.pdu.num_cs = get_NCS(fd->prach_zero_corr_conf,
                              table.format & 0xff,
                              cfg->restricted_set_config);
    rx_nr_prach_ru(&item, time_rx, (NR_DL_FRAME_PARMS *)fp,
                   set_default_nta_offset(fp->freq_range, fp->samples_per_subframe), false);
    for (int td = 0; td < o.num_occasions; ++td) {
      const rx_prach_out_t res = rx_nr_prach(&item, td);
      const int energy = res.max_preamble_energy;
      const int threshold = noise_db10 < 0 ? 250 : (noise_db10 + 80 > 250 ? noise_db10 + 80 : 250);
      if (energy < threshold) {
        noise_db10 = noise_db10 < 0 ? energy : (7 * noise_db10 + energy) / 8;
        continue;
      }
      prach_detection_t *d = &pending[pending_head++ % PRACH_PENDING];
      if (d->valid) emit_detection(d, now);
      const uint64_t stamp = nr_cfg_reconf_enabled() ? nr_cfg_epoch_cfr_stamp() : UINT64_MAX;
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      *d = (prach_detection_t){
          .valid = true, .abs_slot = now - (proc->nr_slot_rx - start_slot),
          .mono_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec,
          .epoch = stamp == UINT64_MAX ? UINT32_MAX : stamp >> 32,
          .identity_gen = stamp, .dedicated_epoch = nr_cfg_reconf_enabled() ? nr_cfg_epoch_dedicated_current() : 0,
          .frame = proc->frame_rx, .slot = start_slot,
          .ra_slot = ra_slot(start_slot, fp->numerology_index, format),
          .slots_per_frame = fp->slots_per_frame, .fdm = fdm,
          .symbol = o.start_symbol + td * o.duration, .preamble = res.max_preamble,
          .energy = energy,
          .delay_samples = (int)lround((double)res.max_preamble_delay * fp->samples_per_subframe
                                       / (1920.0 * (1 << fp->numerology_index))),
          .pci = fp->Nid_cell};
    }
  }
}

void nr_prach_passive_process(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc)
{
  if (!nr_isac_prach_enabled()) return;
  if (atomic_flag_test_and_set_explicit(&prach_busy, memory_order_acquire)) return;
  process_slot(ue, proc);
  atomic_flag_clear_explicit(&prach_busy, memory_order_release);
}
