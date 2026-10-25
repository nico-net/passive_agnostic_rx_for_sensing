#include "nr_passive_cfg_epoch.h"
#include "common/utils/LOG/log.h"
#include "common/utils/nr_cell.h"
#include <pthread.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define NR_CFG_LISTENERS_MAX 16

typedef struct {
  bool valid;
  uint16_t pci;
  uint64_t ssb_arfcn, point_a;
} cell_identity_t;
typedef struct {
  bool valid;
  uint32_t hash;
} hash_state_t;
typedef struct {
  bool active;
  uint64_t boundary;
} si_pending_t;
typedef struct {
  bool valid;
  uint16_t rnti;
  uint64_t slot;
} reopened_t;
typedef struct epoch_event {
  struct epoch_event *next;
  bool bumped;
  bool has_gap;
  int64_t gap_samples;
  uint64_t samples_per_ms;
  nr_cfg_epoch_snapshot_t snapshot;
  nr_cfg_epoch_listener_t listeners[NR_CFG_LISTENERS_MAX];
  unsigned n_listeners;
} epoch_event_t;

typedef nr_cfg_epoch_listener_t g_listeners_arr_t[NR_CFG_LISTENERS_MAX];
NR_CELL_STATIC_INIT(pthread_mutex_t, g_lock, PTHREAD_MUTEX_INITIALIZER);
#define g_lock NR_CELL_REF(g_lock)
NR_CELL_STATIC(_Atomic uint32_t, g_epoch);
#define g_epoch NR_CELL_REF(g_epoch)
NR_CELL_STATIC(_Atomic uint64_t, g_cfr_stamp);
#define g_cfr_stamp NR_CELL_REF(g_cfr_stamp)
NR_CELL_STATIC(_Atomic uint32_t, g_identity_gen);
#define g_identity_gen NR_CELL_REF(g_identity_gen)
NR_CELL_STATIC(_Atomic uint32_t, g_dedicated_epoch);
#define g_dedicated_epoch NR_CELL_REF(g_dedicated_epoch)
NR_CELL_STATIC(_Atomic uint64_t, g_current_slot);
#define g_current_slot NR_CELL_REF(g_current_slot)
NR_CELL_STATIC(nr_cfg_epoch_snapshot_t, g_state);
#define g_state NR_CELL_REF(g_state)
NR_CELL_STATIC(cell_identity_t, g_identity);
#define g_identity NR_CELL_REF(g_identity)
NR_CELL_STATIC(hash_state_t, g_mib);
#define g_mib NR_CELL_REF(g_mib)
NR_CELL_STATIC(hash_state_t, g_sib1);
#define g_sib1 NR_CELL_REF(g_sib1)
NR_CELL_STATIC(si_pending_t, g_pending);
#define g_pending NR_CELL_REF(g_pending)
NR_CELL_STATIC(reopened_t, g_reopened);
#define g_reopened NR_CELL_REF(g_reopened)
NR_CELL_STATIC_INIT(uint32_t, g_slots_per_second, 1000);
#define g_slots_per_second NR_CELL_REF(g_slots_per_second)
NR_CELL_STATIC(_Atomic uint32_t, g_si_period_slots);
#define g_si_period_slots NR_CELL_REF(g_si_period_slots)
NR_CELL_STATIC(uint64_t, g_last_abs_frame);
#define g_last_abs_frame NR_CELL_REF(g_last_abs_frame)
NR_CELL_STATIC(bool, g_have_sfn);
#define g_have_sfn NR_CELL_REF(g_have_sfn)
NR_CELL_STATIC(g_listeners_arr_t, g_listeners);
#define g_listeners NR_CELL_REF(g_listeners)
NR_CELL_STATIC(unsigned, g_n_listeners);
#define g_n_listeners NR_CELL_REF(g_n_listeners)
NR_CELL_STATIC(epoch_event_t *, g_event_head);
#define g_event_head NR_CELL_REF(g_event_head)
NR_CELL_STATIC(epoch_event_t *, g_event_tail);
#define g_event_tail NR_CELL_REF(g_event_tail)
NR_CELL_STATIC_INIT(atomic_flag, g_draining, ATOMIC_FLAG_INIT);
#define g_draining NR_CELL_REF(g_draining)
static _Thread_local nr_cfg_epoch_work_t *current_work;
static pthread_once_t enable_once = PTHREAD_ONCE_INIT;
static bool enabled;
enum { si_grace_ms = 5000 }; /* SI-modification boundary grace before the bump */

static void read_enable(void)
{
  const char *s = getenv("ISAC_RECONF");
  enabled = s && strcmp(s, "1") == 0;
}
static bool is_enabled(void)
{
  pthread_once(&enable_once, read_enable);
  return enabled;
}
bool nr_cfg_reconf_enabled(void) { return is_enabled(); }
/* The SIB1-less test arm is gone; the API stays for the callers outside this file. */
bool nr_cfg_ignore_sib1(void) { return false; }
void nr_cfg_ignore_sib1_reset_for_test(void) {}
void nr_cfg_si_options_reset_for_test(void) {}
static const char *class_name(nr_epoch_class_t c)
{
  switch (c) {
    case NR_EPOCH_SOFT: return "SOFT";
    case NR_EPOCH_HARD_REVERIFY: return "HARD_REVERIFY";
    case NR_EPOCH_HARD_RESET: return "HARD_RESET";
  }
  return "UNKNOWN";
}
/* cause_name: returns the printable name of an epoch cause (for example "CSIRS_MAP_CHANGE"), or "UNKNOWN". Static. Caller: the epoch log lines in this file. */
static const char *cause_name(nr_epoch_cause_t c)
{
  switch (c) {
    case NR_CAUSE_CELL_IDENTITY_CHANGE: return "CELL_IDENTITY_CHANGE";
    case NR_CAUSE_MIB_CHANGE: return "MIB_CHANGE";
    case NR_CAUSE_SIB1_CHANGE: return "SIB1_CHANGE";
    case NR_CAUSE_SI_MODIFICATION_ANNOUNCED: return "SI_MODIFICATION_ANNOUNCED";
    case NR_CAUSE_CONTINUITY_LOSS: return "CONTINUITY_LOSS";
    case NR_CAUSE_BWP_CHANGE: return "BWP_CHANGE";
    case NR_CAUSE_DEDICATED_CHANGE_SUSPECTED: return "DEDICATED_CHANGE_SUSPECTED";
    case NR_CAUSE_CSIRS_MAP_CHANGE: return "CSIRS_MAP_CHANGE";
    case NR_CAUSE_RX_RECOVERY: return "RX_RECOVERY";
  }
  return "UNKNOWN";
}
static void bump_locked(epoch_event_t *event, nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  g_state.epoch++;
  if (cls == NR_EPOCH_HARD_RESET) g_state.identity_gen++;
  g_state.last_class = cls;
  g_state.last_cause = cause;
  if (cls != NR_EPOCH_SOFT || cause == NR_CAUSE_DEDICATED_CHANGE_SUSPECTED)
    g_state.dedicated_epoch = g_state.epoch;
  atomic_store_explicit(&g_dedicated_epoch, g_state.dedicated_epoch, memory_order_release);
  atomic_store_explicit(&g_epoch, g_state.epoch, memory_order_release);
  atomic_store_explicit(&g_identity_gen, g_state.identity_gen, memory_order_release);
  atomic_store_explicit(&g_cfr_stamp, ((uint64_t)g_state.epoch << 32) | g_state.identity_gen, memory_order_release);
  event->bumped = true;
  event->snapshot = g_state;
  event->n_listeners = g_n_listeners;
  memcpy(event->listeners, g_listeners, g_n_listeners * sizeof(g_listeners[0]));
  epoch_event_t *queued = malloc(sizeof(*queued));
  AssertFatal(queued, "epoch event allocation failed\n");
  *queued = *event;
  queued->next = NULL;
  if (g_event_tail) g_event_tail->next = queued;
  else g_event_head = queued;
  g_event_tail = queued;
}
static void dispatch(const epoch_event_t *event)
{
  if (!event->bumped) return;
  const nr_cfg_epoch_snapshot_t *s = &event->snapshot;
  LOG_I(PHY, "SENSING: CONFIG_EPOCH %u -> %u class=%s cause=%s scope=%s\n",
        s->epoch - 1, s->epoch, class_name(s->last_class), cause_name(s->last_cause),
        s->last_class == NR_EPOCH_HARD_RESET ? "cell_identity" : "cell");
  if (event->has_gap)
    LOG_I(PHY, "SENSING: CONFIG_EPOCH continuity gap_samples=%" PRId64 " gap_ms=%.3f\n",
          event->gap_samples, (double)event->gap_samples / event->samples_per_ms);
  for (unsigned i = 0; i < event->n_listeners; ++i) event->listeners[i](s);
}

/* Only call from a point with no receiver locks held. One drainer preserves bump order;
 * callbacks may themselves enqueue bumps or attempt a recursive drain. */
void nr_cfg_epoch_drain(void)
{
  if (atomic_flag_test_and_set_explicit(&g_draining, memory_order_acquire)) return;
  for (;;) {
    pthread_mutex_lock(&g_lock);
    epoch_event_t *event = g_event_head;
    if (event) {
      g_event_head = event->next;
      if (!g_event_head) g_event_tail = NULL;
    }
    pthread_mutex_unlock(&g_lock);
    if (!event) break;
    nr_cfg_epoch_work_t *saved_work = current_work;
    current_work = NULL;
    dispatch(event);
    current_work = saved_work;
    free(event);
  }
  atomic_flag_clear_explicit(&g_draining, memory_order_release);
}

void nr_cfg_epoch_work_begin(nr_cfg_epoch_work_t *work, uint32_t stamp, void *counter)
{
  *work = (nr_cfg_epoch_work_t){.parent = current_work, .epoch = stamp, .dropped_counter = counter};
  work->owner = current_work && current_work->epoch == stamp
      && (!counter || counter == current_work->owner->dropped_counter) ? current_work->owner : work;
  current_work = work;
}
void nr_cfg_epoch_work_end(nr_cfg_epoch_work_t *work) { current_work = work->parent; }
uint32_t nr_cfg_epoch_work_stamp(void)
{
  return current_work ? current_work->epoch : nr_cfg_epoch_current();
}
bool nr_cfg_epoch_work_current(void)
{
  if (!is_enabled() || !current_work || current_work->epoch == nr_cfg_epoch_current()) return true;
  nr_cfg_epoch_work_t *owner = current_work->owner;
  if (!owner->counted) {
    owner->counted = true;
    if (owner->dropped_counter) __atomic_fetch_add((uint64_t *)owner->dropped_counter, 1, __ATOMIC_RELAXED);
  }
  return false;
}

uint32_t nr_cfg_epoch_current(void) { return atomic_load_explicit(&g_epoch, memory_order_acquire); }
uint32_t nr_cfg_epoch_identity_gen(void) { return atomic_load_explicit(&g_identity_gen, memory_order_acquire); }
uint32_t nr_cfg_epoch_dedicated_current(void) { return atomic_load_explicit(&g_dedicated_epoch, memory_order_acquire); }
uint64_t nr_cfg_epoch_current_slot(void) { return atomic_load_explicit(&g_current_slot, memory_order_acquire); }
uint64_t nr_cfg_epoch_cfr_stamp(void) { return atomic_load_explicit(&g_cfr_stamp, memory_order_acquire); }
nr_cfg_epoch_snapshot_t nr_cfg_epoch_snapshot(void)
{
  pthread_mutex_lock(&g_lock);
  nr_cfg_epoch_snapshot_t result = g_state;
  pthread_mutex_unlock(&g_lock);
  return result;
}
void nr_cfg_epoch_reset(void)
{
  pthread_mutex_lock(&g_lock);
  while (g_event_head) {
    epoch_event_t *next = g_event_head->next;
    free(g_event_head);
    g_event_head = next;
  }
  g_event_tail = NULL;
  memset(&g_state, 0, sizeof(g_state));
  atomic_store_explicit(&g_cfr_stamp, 0, memory_order_release);
  memset(&g_identity, 0, sizeof(g_identity));
  memset(&g_mib, 0, sizeof(g_mib));
  memset(&g_sib1, 0, sizeof(g_sib1));
  memset(&g_pending, 0, sizeof(g_pending));
  memset(&g_reopened, 0, sizeof(g_reopened));
  memset(g_listeners, 0, sizeof(g_listeners));
  g_n_listeners = 0;
  g_slots_per_second = 1000;
  atomic_store_explicit(&g_si_period_slots, 0, memory_order_release);
  g_last_abs_frame = 0;
  g_have_sfn = false;
  atomic_store_explicit(&g_dedicated_epoch, 0, memory_order_release);
  atomic_store_explicit(&g_current_slot, 0, memory_order_release);
  atomic_store_explicit(&g_epoch, 0, memory_order_release);
  atomic_store_explicit(&g_identity_gen, 0, memory_order_release);
  pthread_mutex_unlock(&g_lock);
}
void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn)
{
  if (!fn) return;
  pthread_mutex_lock(&g_lock);
  for (unsigned i = 0; i < g_n_listeners; ++i)
    if (g_listeners[i] == fn) { pthread_mutex_unlock(&g_lock); return; }
  if (g_n_listeners < NR_CFG_LISTENERS_MAX) g_listeners[g_n_listeners++] = fn;
  else LOG_W(PHY, "SENSING: CONFIG_EPOCH listener capacity exhausted\n");
  pthread_mutex_unlock(&g_lock);
}
void nr_cfg_epoch_set_slots_per_second(uint32_t value)
{
  if (!value) return;
  pthread_mutex_lock(&g_lock);
  g_slots_per_second = value;
  g_reopened.valid = false;
  pthread_mutex_unlock(&g_lock);
}
void nr_cfg_epoch_set_si_period(uint32_t value)
{
  if (is_enabled()) atomic_store_explicit(&g_si_period_slots, value, memory_order_release);
}
uint32_t nr_cfg_epoch_si_period(void)
{
  return atomic_load_explicit(&g_si_period_slots, memory_order_acquire);
}
uint64_t nr_cfg_epoch_si_boundary(void)
{
  pthread_mutex_lock(&g_lock);
  const uint64_t boundary = g_pending.active ? g_pending.boundary : 0;
  pthread_mutex_unlock(&g_lock);
  return boundary;
}
bool nr_cfg_epoch_sib1_request_allowed(uint64_t abs_slot)
{
  pthread_mutex_lock(&g_lock);
  const bool allowed = !g_pending.active || abs_slot >= g_pending.boundary;
  pthread_mutex_unlock(&g_lock);
  return allowed;
}
bool nr_cfg_epoch_si_redecode_pending(uint64_t abs_slot)
{
  if (!is_enabled()) return false;
  pthread_mutex_lock(&g_lock);
  const bool needed = g_pending.active && abs_slot >= g_pending.boundary;
  pthread_mutex_unlock(&g_lock);
  return needed;
}
uint64_t nr_cfg_epoch_observe_slot(uint16_t sfn, uint8_t slot, uint8_t slots_per_frame)
{
  if (!slots_per_frame || sfn >= 1024 || slot >= slots_per_frame) return 0;
  pthread_mutex_lock(&g_lock);
  uint64_t frame = g_have_sfn ? (g_last_abs_frame / 1024u) * 1024u + sfn : sfn;
  if (g_have_sfn && frame + 512u < g_last_abs_frame) frame += 1024u;
  else if (g_have_sfn && frame > g_last_abs_frame + 512u && frame >= 1024u) frame -= 1024u;
  if (!g_have_sfn || frame > g_last_abs_frame) g_last_abs_frame = frame;
  g_have_sfn = true;
  const uint64_t absolute = frame * slots_per_frame + slot;
  pthread_mutex_unlock(&g_lock);
  return absolute;
}
void nr_cfg_epoch_note_identity(uint16_t pci, uint64_t ssb_arfcn, uint64_t point_a)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  const bool changed = g_identity.valid && (g_identity.pci != pci || g_identity.ssb_arfcn != ssb_arfcn
      || (point_a && g_identity.point_a && g_identity.point_a != point_a));
  if (changed) {
    memset(&g_mib, 0, sizeof(g_mib));
    memset(&g_sib1, 0, sizeof(g_sib1));
    memset(&g_pending, 0, sizeof(g_pending));
    g_reopened.valid = false;
    atomic_store_explicit(&g_si_period_slots, 0, memory_order_release);
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  g_identity = (cell_identity_t){true, pci, ssb_arfcn, point_a ? point_a : (changed ? 0 : g_identity.point_a)};
  pthread_mutex_unlock(&g_lock);

}
void nr_cfg_epoch_refine_point_a(uint64_t point_a)
{
  if (!is_enabled() || !point_a) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  if (g_identity.valid && g_identity.point_a && g_identity.point_a != point_a) {
    memset(&g_mib, 0, sizeof(g_mib));
    memset(&g_sib1, 0, sizeof(g_sib1));
    memset(&g_pending, 0, sizeof(g_pending));
    g_reopened.valid = false;
    atomic_store_explicit(&g_si_period_slots, 0, memory_order_release);
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  if (g_identity.valid) g_identity.point_a = point_a;
  pthread_mutex_unlock(&g_lock);

}
void nr_cfg_epoch_note_mib(uint32_t hash)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  if (g_mib.valid && g_mib.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_MIB_CHANGE);
  g_mib = (hash_state_t){true, hash};
  pthread_mutex_unlock(&g_lock);

}
bool nr_cfg_epoch_note_sib1(uint32_t hash, uint64_t abs_slot)
{
  if (!is_enabled()) return true;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  if (g_pending.active && abs_slot < g_pending.boundary) {
    pthread_mutex_unlock(&g_lock);
    return false; /* old-period SI cannot complete the requested comparison */
  }
  if (g_sib1.valid && g_sib1.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SIB1_CHANGE);
  g_sib1 = (hash_state_t){true, hash};
  g_pending.active = false;
  pthread_mutex_unlock(&g_lock);
  return true;
}
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t period)
{
  if (!is_enabled() || !period) return;
  pthread_mutex_lock(&g_lock);
  /* TS 38.331 5.2.2.2.2 defines SFN mod m = 0, not an arbitrary unwrapped
   * frame origin. For m > 1024 the only representable boundary SFN is zero. */
  const uint32_t sfn_cycle = 1024u * (g_slots_per_second / 100u);
  if (sfn_cycle && period > sfn_cycle) period = sfn_cycle;
  const uint64_t next_boundary = (abs_slot / period + 1) * (uint64_t)period;
  if (g_pending.active && g_pending.boundary == next_boundary) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  g_pending.active = true;
  g_pending.boundary = next_boundary;
  const uint64_t boundary = g_pending.boundary;
  pthread_mutex_unlock(&g_lock);
  LOG_I(PHY, "SENSING: CONFIG_EPOCH pending cause=SI_MODIFICATION_ANNOUNCED boundary=%lu\n",
        (unsigned long)boundary);
}
void nr_cfg_epoch_tick(uint64_t abs_slot)
{
  if (!is_enabled()) return;
  atomic_store_explicit(&g_current_slot, abs_slot, memory_order_release);
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  if (g_pending.active && abs_slot >= g_pending.boundary
      && abs_slot - g_pending.boundary >= (uint64_t)si_grace_ms * g_slots_per_second / 1000) {
    bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
    g_pending.active = false;
  }
  pthread_mutex_unlock(&g_lock);

}
static void simple_bump(nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  bump_locked(&event, cls, cause);
  pthread_mutex_unlock(&g_lock);

}
void nr_cfg_epoch_note_continuity_loss(void) { simple_bump(NR_EPOCH_HARD_REVERIFY, NR_CAUSE_CONTINUITY_LOSS); }
/* nr_cfg_epoch_note_rx_recovery: records a soft configuration epoch caused by an RX recovery (the samples were restarted, the configuration is kept). Caller: executables/nr-ue.c (RX recovery after a stream restart). */
void nr_cfg_epoch_note_rx_recovery(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_RX_RECOVERY); }
void nr_cfg_epoch_note_continuity_loss_samples(int64_t gap_samples, uint64_t samples_per_ms)
{
  if (!is_enabled()) return;
  const uint64_t threshold_ms = 10;
  const bool hard = gap_samples <= 0 || !samples_per_ms ||
      (threshold_ms <= UINT64_MAX / samples_per_ms && (uint64_t)gap_samples >= threshold_ms * samples_per_ms);
  epoch_event_t event = {.has_gap = true, .gap_samples = gap_samples,
                         .samples_per_ms = samples_per_ms ? samples_per_ms : 1};
  pthread_mutex_lock(&g_lock);
  bump_locked(&event, hard ? NR_EPOCH_HARD_REVERIFY : NR_EPOCH_SOFT, NR_CAUSE_CONTINUITY_LOSS);
  pthread_mutex_unlock(&g_lock);
}
void nr_cfg_epoch_note_bwp_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_BWP_CHANGE); }
void nr_cfg_epoch_note_csirs_map_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_CSIRS_MAP_CHANGE); }
void nr_cfg_epoch_note_rnti_reopened(uint16_t rnti, bool was_converged, uint64_t abs_slot)
{
  if (!is_enabled() || !was_converged) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&g_lock);
  const uint64_t window = 2u * (uint64_t)g_slots_per_second;
  if (g_reopened.valid && g_reopened.rnti != rnti && abs_slot >= g_reopened.slot && abs_slot - g_reopened.slot <= window) {
    bump_locked(&event, NR_EPOCH_SOFT, NR_CAUSE_DEDICATED_CHANGE_SUSPECTED);
    g_reopened.valid = false;
  } else if (!g_reopened.valid || g_reopened.rnti != rnti || abs_slot < g_reopened.slot || abs_slot - g_reopened.slot > window) {
    g_reopened = (reopened_t){true, rnti, abs_slot};
  }
  pthread_mutex_unlock(&g_lock);

}
