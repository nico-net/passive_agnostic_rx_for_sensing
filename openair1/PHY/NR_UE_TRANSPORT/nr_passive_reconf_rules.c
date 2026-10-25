/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_passive_reconf_rules.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "common/utils/nr_cell.h"
#include "nr_passive_cell_prealloc.h"

static unsigned bits(const uint8_t *b, unsigned pos, unsigned n) /* MSB-first bit field, n <= 32 */
{
  unsigned v = 0;
  for (unsigned i = 0; i < n; i++) v = (v << 1) | ((b[(pos + i) >> 3] >> (7 - ((pos + i) & 7))) & 1u);
  return v;
}

/* TS 38.331 UL-CCCH-Message ::= CHOICE { c1 CHOICE { rrcSetupRequest, rrcResumeRequest, rrcReestablishmentRequest, rrcSystemInfoRequest }, messageClassExtension }
 * (UPER: 1 bit for the outer choice, 2 bits for c1). RRCSetupRequest-IEs: ue-Identity CHOICE (1 bit) {ng-5G-S-TMSI-Part1 | randomValue} 39 bits,
 * establishmentCause 4 bits, spare 1 bit = 48 bits. RRCResumeRequest-IEs: resumeIdentity ShortI-RNTI-Value 24 bits, resumeMAC-I 16, resumeCause 4,
 * spare 1 = 48 bits. */
bool nr_reconf_ccch_identity(const uint8_t sdu[6], nr_reconf_id_t *out)
{
  if (!sdu || !out) return false;
  *out = (nr_reconf_id_t){0};
  if (bits(sdu, 0, 1) != 0) return false; /* messageClassExtension */
  const unsigned msg = bits(sdu, 1, 2);
  if (msg == 0) {
    out->kind = bits(sdu, 3, 1) == 0 ? NR_RECONF_ID_SETUP_STMSI : NR_RECONF_ID_SETUP_RANDOM;
    out->value = ((uint64_t)bits(sdu, 4, 20) << 19) | bits(sdu, 24, 19);
    return true;
  }
  if (msg == 1) {
    out->kind = NR_RECONF_ID_RESUME;
    out->value = bits(sdu, 3, 24);
    return true;
  }
  return false;
}
bool nr_reconf_id_linkable(const nr_reconf_id_t *id)
{
  return id && (id->kind == NR_RECONF_ID_SETUP_STMSI || id->kind == NR_RECONF_ID_RESUME);
}
bool nr_reconf_id_equal(const nr_reconf_id_t *a, const nr_reconf_id_t *b)
{
  return a && b && a->kind != NR_RECONF_ID_NONE && a->kind == b->kind && a->value == b->value;
}
bool nr_reconf_cr_matches(const uint8_t msg3_ccch[6], const uint8_t cr_identity[6])
{
  return msg3_ccch && cr_identity && memcmp(msg3_ccch, cr_identity, 6) == 0;
}
bool nr_reconf_ulsch_ccch(const uint8_t *pdu, uint32_t len, uint8_t out[6])
{
  uint32_t i = 0;
  while (pdu && i < len) {
    const uint8_t lcid = pdu[i] & 0x3F;
    i++;
    uint32_t sz;
    switch (lcid) {
      case 52: sz = 6; break;                     /* CCCH, 48 bits */
      case 0: sz = 8; break;                      /* CCCH1, 64 bits */
      case 58: sz = 2; break;                     /* C-RNTI */
      case 59: case 61: sz = 1; break;            /* short (truncated) BSR */
      case 57: sz = 2; break;                     /* single entry PHR */
      case 55: sz = 0; break;                     /* configured grant confirmation */
      default: return false;                      /* padding, variable-length or unknown: stop */
    }
    if (len - i < sz) return false;
    if (lcid == 52 || lcid == 0) {
      if (out) memcpy(out, pdu + i, 6);
      return true;
    }
    i += sz;
  }
  return false;
}
uint64_t nr_reconf_pack48(const uint8_t b[6])
{
  uint64_t v = 0;
  for (int i = 0; i < 6; i++) v = (v << 8) | b[i];
  return v;
}
void nr_reconf_unpack48(uint64_t v, uint8_t b[6])
{
  for (int i = 5; i >= 0; i--, v >>= 8) b[i] = (uint8_t)v;
}

int nr_reconf_bwp_switch_delay_slots(int mu, int type)
{
  static const uint8_t t1[4] = {1, 2, 3, 6}, t2[4] = {3, 5, 9, 18}; /* TS 38.133 Table 8.6.2-1, FR1 */
  if (mu < 0 || mu > 3) return -1;
  return type == 1 ? t1[mu] : t2[mu];
}
int nr_reconf_rrc_delay_slots(nr_reconf_rrc_kind_t kind, int mu, bool with_bwp_switch)
{
  if (mu < 0 || mu > 3) return -1;
  const int ms = kind == NR_RECONF_RRC_RECONF ? 16 : 10; /* TS 38.331 12 */
  return (ms << mu) + (with_bwp_switch ? nr_reconf_bwp_switch_delay_slots(mu, 2) : 0);
}
bool nr_reconf_hq_discontinuity(bool prev_valid, uint8_t prev_ndi, int prev_tbs_bits, uint8_t ndi, int tbs_bits)
{
  return prev_valid && prev_ndi == ndi && prev_tbs_bits > 0 && tbs_bits > 0 && prev_tbs_bits != tbs_bits;
}

/* ---- RRC windows ---- */
typedef _Atomic int64_t g_rrc_arr_t[65536]; /* slot + 1; 0 = none; 512 KiB per cell: heap, pre-allocated per active cell before the RX fence */
NR_CELL_STATIC_HEAP(g_rrc_arr_t, g_rrc_rx);
#define g_rrc_rx NR_CELL_HEAP_REF(g_rrc_arr_t, g_rrc_rx)
NR_CELL_STATIC_HEAP(g_rrc_arr_t, g_rrc_eff);
#define g_rrc_eff NR_CELL_HEAP_REF(g_rrc_arr_t, g_rrc_eff)
void nr_reconf_rrc_expect(uint16_t rnti, int64_t rx_slot, nr_reconf_rrc_kind_t kind, int mu, bool with_bwp_switch)
{
  const int d = nr_reconf_rrc_delay_slots(kind, mu, with_bwp_switch);
  if (rnti == 0 || d < 0 || rx_slot < 0) return;
  atomic_store_explicit(&g_rrc_rx[rnti], rx_slot + 1, memory_order_relaxed);
  atomic_store_explicit(&g_rrc_eff[rnti], rx_slot + d + 1, memory_order_release);
}
int64_t nr_reconf_rrc_effective_slot(uint16_t rnti)
{
  return atomic_load_explicit(&g_rrc_eff[rnti], memory_order_acquire) - 1;
}
nr_reconf_phase_t nr_reconf_rrc_phase(uint16_t rnti, int64_t abs_slot, int mu)
{
  const int64_t eff = atomic_load_explicit(&g_rrc_eff[rnti], memory_order_acquire) - 1;
  const int64_t rx = atomic_load_explicit(&g_rrc_rx[rnti], memory_order_relaxed) - 1;
  if (eff < 0 || rx < 0 || abs_slot < rx || mu < 0 || mu > 3) return NR_RECONF_PHASE_NONE;
  if (abs_slot < eff) return NR_RECONF_PHASE_HOLD;
  return abs_slot < eff + ((int64_t)NR_RECONF_ARM_MS << mu) ? NR_RECONF_PHASE_ARMED : NR_RECONF_PHASE_NONE;
}
uint32_t nr_reconf_rrc_n_suspect(uint16_t rnti, int64_t abs_slot, int mu, uint32_t base)
{
  switch (nr_reconf_rrc_phase(rnti, abs_slot, mu)) {
    case NR_RECONF_PHASE_HOLD: return base; /* the new configuration may apply in any slot of the window: misses stay valid evidence, no premature verdict is forced */
    case NR_RECONF_PHASE_ARMED: return base < NR_RECONF_ARM_N_SUSPECT ? base : NR_RECONF_ARM_N_SUSPECT;
    default: return base;
  }
}

/* ---- DCI-triggered BWP switches ---- */
#define PENDING_MAX 16
typedef struct { bool used; uint16_t rnti; int entry; int64_t first, last; } g_sw_ent_t;
typedef g_sw_ent_t g_sw_arr_t[PENDING_MAX];
NR_CELL_STATIC(g_sw_arr_t, g_sw);
#define g_sw NR_CELL_REF(g_sw)
NR_CELL_STATIC_INIT(pthread_mutex_t, g_sw_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_sw_lock NR_CELL_REF(g_sw_lock)

/* Allocate cell `cell`'s heap objects (call for every active cell before nr_cell_alloc_fence(); cell 0 included). */
void nr_passive_reconf_rules_cell_prealloc(int cell)
{
  NR_CELL_HEAP_PREALLOC(g_rrc_arr_t, g_rrc_rx, cell);
  NR_CELL_HEAP_PREALLOC(g_rrc_arr_t, g_rrc_eff, cell);
}
void nr_reconf_bwp_switch_dci(uint16_t rnti, int to_entry, int64_t dci_slot, int mu)
{
  const int d1 = nr_reconf_bwp_switch_delay_slots(mu, 1), d2 = nr_reconf_bwp_switch_delay_slots(mu, 2);
  if (rnti == 0 || d1 < 0 || dci_slot < 0) return;
  pthread_mutex_lock(&g_sw_lock);
  int slot = -1;
  for (int i = 0; i < PENDING_MAX; i++)
    if (g_sw[i].used && g_sw[i].rnti == rnti) { slot = i; break; }
  for (int i = 0; slot < 0 && i < PENDING_MAX; i++)
    if (!g_sw[i].used) slot = i;
  if (slot < 0) { /* full: replace the oldest */
    slot = 0;
    for (int i = 1; i < PENDING_MAX; i++)
      if (g_sw[i].first < g_sw[slot].first) slot = i;
  }
  g_sw[slot].used = true;
  g_sw[slot].rnti = rnti;
  g_sw[slot].entry = to_entry;
  g_sw[slot].first = dci_slot + d1;
  g_sw[slot].last = dci_slot + d2;
  pthread_mutex_unlock(&g_sw_lock);
}
bool nr_reconf_bwp_due(int64_t abs_slot, uint16_t *rnti, int *entry)
{
  bool hit = false;
  pthread_mutex_lock(&g_sw_lock);
  for (int i = 0; i < PENDING_MAX && !hit; i++)
    if (g_sw[i].used && abs_slot >= g_sw[i].last) {
      if (rnti) *rnti = g_sw[i].rnti;
      if (entry) *entry = g_sw[i].entry;
      g_sw[i].used = false;
      hit = true;
    }
  pthread_mutex_unlock(&g_sw_lock);
  return hit;
}
bool nr_reconf_bwp_expected_window(uint16_t rnti, int64_t *first, int64_t *last)
{
  bool hit = false;
  pthread_mutex_lock(&g_sw_lock);
  for (int i = 0; i < PENDING_MAX && !hit; i++)
    if (g_sw[i].used && g_sw[i].rnti == rnti) {
      if (first) *first = g_sw[i].first;
      if (last) *last = g_sw[i].last;
      hit = true;
    }
  pthread_mutex_unlock(&g_sw_lock);
  return hit;
}
void nr_reconf_reset(void)
{
  for (int i = 0; i < 65536; i++) {
    atomic_store(&g_rrc_rx[i], 0);
    atomic_store(&g_rrc_eff[i], 0);
  }
  pthread_mutex_lock(&g_sw_lock);
  memset(g_sw, 0, sizeof(g_sw));
  pthread_mutex_unlock(&g_sw_lock);
}
