#include "nr_passive_cfg_epoch.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
#include <errno.h>
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

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t epoch;
static _Atomic uint32_t identity_gen;
static nr_cfg_epoch_snapshot_t state;
static cell_identity_t identity;
static hash_state_t mib, sib1;
static si_pending_t pending;
static reopened_t reopened;
static uint32_t slots_per_second = 1000;
static _Atomic uint32_t si_period_slots;
static uint64_t last_abs_frame;
static bool have_sfn;
static nr_cfg_epoch_listener_t listeners[NR_CFG_LISTENERS_MAX];
static unsigned n_listeners;
static epoch_event_t *event_head, *event_tail;
static atomic_flag draining = ATOMIC_FLAG_INIT;
static _Thread_local nr_cfg_epoch_work_t *current_work;
static pthread_once_t enable_once = PTHREAD_ONCE_INIT;
static bool enabled;

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
bool nr_cfg_ignore_sib1(void)
{
  const char *value = getenv("ISAC_TD_IGNORE_SIB1");
  return value && strcmp(value, "1") == 0;
}
static const char *class_name(nr_epoch_class_t c)
{
  switch (c) {
    case NR_EPOCH_SOFT: return "SOFT";
    case NR_EPOCH_HARD_REVERIFY: return "HARD_REVERIFY";
    case NR_EPOCH_HARD_RESET: return "HARD_RESET";
  }
  return "UNKNOWN";
}
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
  }
  return "UNKNOWN";
}
static void bump_locked(epoch_event_t *event, nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  state.epoch++;
  if (cls == NR_EPOCH_HARD_RESET) state.identity_gen++;
  state.last_class = cls;
  state.last_cause = cause;
  atomic_store_explicit(&epoch, state.epoch, memory_order_release);
  atomic_store_explicit(&identity_gen, state.identity_gen, memory_order_release);
  event->bumped = true;
  event->snapshot = state;
  event->n_listeners = n_listeners;
  memcpy(event->listeners, listeners, n_listeners * sizeof(listeners[0]));
  epoch_event_t *queued = malloc(sizeof(*queued));
  AssertFatal(queued, "epoch event allocation failed\n");
  *queued = *event;
  queued->next = NULL;
  if (event_tail) event_tail->next = queued;
  else event_head = queued;
  event_tail = queued;
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
  if (atomic_flag_test_and_set_explicit(&draining, memory_order_acquire)) return;
  for (;;) {
    pthread_mutex_lock(&lock);
    epoch_event_t *event = event_head;
    if (event) {
      event_head = event->next;
      if (!event_head) event_tail = NULL;
    }
    pthread_mutex_unlock(&lock);
    if (!event) break;
    nr_cfg_epoch_work_t *saved_work = current_work;
    current_work = NULL;
    dispatch(event);
    current_work = saved_work;
    free(event);
  }
  atomic_flag_clear_explicit(&draining, memory_order_release);
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

uint32_t nr_cfg_epoch_current(void) { return atomic_load_explicit(&epoch, memory_order_acquire); }
uint32_t nr_cfg_epoch_identity_gen(void) { return atomic_load_explicit(&identity_gen, memory_order_acquire); }
nr_cfg_epoch_snapshot_t nr_cfg_epoch_snapshot(void)
{
  pthread_mutex_lock(&lock);
  nr_cfg_epoch_snapshot_t result = state;
  pthread_mutex_unlock(&lock);
  return result;
}
void nr_cfg_epoch_reset(void)
{
  pthread_mutex_lock(&lock);
  while (event_head) {
    epoch_event_t *next = event_head->next;
    free(event_head);
    event_head = next;
  }
  event_tail = NULL;
  memset(&state, 0, sizeof(state));
  memset(&identity, 0, sizeof(identity));
  memset(&mib, 0, sizeof(mib));
  memset(&sib1, 0, sizeof(sib1));
  memset(&pending, 0, sizeof(pending));
  memset(&reopened, 0, sizeof(reopened));
  memset(listeners, 0, sizeof(listeners));
  n_listeners = 0;
  slots_per_second = 1000;
  atomic_store_explicit(&si_period_slots, 0, memory_order_release);
  last_abs_frame = 0;
  have_sfn = false;
  atomic_store_explicit(&epoch, 0, memory_order_release);
  atomic_store_explicit(&identity_gen, 0, memory_order_release);
  pthread_mutex_unlock(&lock);
}
void nr_cfg_epoch_subscribe(nr_cfg_epoch_listener_t fn)
{
  if (!fn) return;
  pthread_mutex_lock(&lock);
  for (unsigned i = 0; i < n_listeners; ++i)
    if (listeners[i] == fn) { pthread_mutex_unlock(&lock); return; }
  if (n_listeners < NR_CFG_LISTENERS_MAX) listeners[n_listeners++] = fn;
  else LOG_W(PHY, "SENSING: CONFIG_EPOCH listener capacity exhausted\n");
  pthread_mutex_unlock(&lock);
}
void nr_cfg_epoch_set_slots_per_second(uint32_t value)
{
  if (!value) return;
  pthread_mutex_lock(&lock);
  slots_per_second = value;
  reopened.valid = false;
  pthread_mutex_unlock(&lock);
}
void nr_cfg_epoch_set_si_period(uint32_t value)
{
  if (nr_cfg_ignore_sib1()) return;
  if (is_enabled()) atomic_store_explicit(&si_period_slots, value, memory_order_release);
}
uint32_t nr_cfg_epoch_si_period(void)
{
  if (nr_cfg_ignore_sib1()) return 0;
  return atomic_load_explicit(&si_period_slots, memory_order_acquire);
}
uint64_t nr_cfg_epoch_si_boundary(void)
{
  if (nr_cfg_ignore_sib1()) return 0;
  pthread_mutex_lock(&lock);
  const uint64_t boundary = pending.active ? pending.boundary : 0;
  pthread_mutex_unlock(&lock);
  return boundary;
}
bool nr_cfg_epoch_sib1_request_allowed(uint64_t abs_slot)
{
  if (nr_cfg_ignore_sib1()) return false;
  pthread_mutex_lock(&lock);
  const bool allowed = !pending.active || abs_slot >= pending.boundary;
  pthread_mutex_unlock(&lock);
  return allowed;
}
bool nr_cfg_epoch_si_redecode_pending(uint64_t abs_slot)
{
  if (!is_enabled() || nr_cfg_ignore_sib1()) return false;
  pthread_mutex_lock(&lock);
  const bool needed = pending.active && abs_slot >= pending.boundary;
  pthread_mutex_unlock(&lock);
  return needed;
}
uint64_t nr_cfg_epoch_observe_slot(uint16_t sfn, uint8_t slot, uint8_t slots_per_frame)
{
  if (!slots_per_frame || sfn >= 1024 || slot >= slots_per_frame) return 0;
  pthread_mutex_lock(&lock);
  uint64_t frame = have_sfn ? (last_abs_frame / 1024u) * 1024u + sfn : sfn;
  if (have_sfn && frame + 512u < last_abs_frame) frame += 1024u;
  else if (have_sfn && frame > last_abs_frame + 512u && frame >= 1024u) frame -= 1024u;
  if (!have_sfn || frame > last_abs_frame) last_abs_frame = frame;
  have_sfn = true;
  const uint64_t absolute = frame * slots_per_frame + slot;
  pthread_mutex_unlock(&lock);
  return absolute;
}
void nr_cfg_epoch_note_identity(uint16_t pci, uint64_t ssb_arfcn, uint64_t point_a)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  const bool changed = identity.valid && (identity.pci != pci || identity.ssb_arfcn != ssb_arfcn
      || (point_a && identity.point_a && identity.point_a != point_a));
  if (changed) {
    memset(&mib, 0, sizeof(mib));
    memset(&sib1, 0, sizeof(sib1));
    memset(&pending, 0, sizeof(pending));
    reopened.valid = false;
    atomic_store_explicit(&si_period_slots, 0, memory_order_release);
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  identity = (cell_identity_t){true, pci, ssb_arfcn, point_a ? point_a : (changed ? 0 : identity.point_a)};
  pthread_mutex_unlock(&lock);

}
void nr_cfg_epoch_refine_point_a(uint64_t point_a)
{
  if (!is_enabled() || nr_cfg_ignore_sib1() || !point_a) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (identity.valid && identity.point_a && identity.point_a != point_a) {
    memset(&mib, 0, sizeof(mib));
    memset(&sib1, 0, sizeof(sib1));
    memset(&pending, 0, sizeof(pending));
    reopened.valid = false;
    atomic_store_explicit(&si_period_slots, 0, memory_order_release);
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  if (identity.valid) identity.point_a = point_a;
  pthread_mutex_unlock(&lock);

}
void nr_cfg_epoch_note_mib(uint32_t hash)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (mib.valid && mib.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_MIB_CHANGE);
  mib = (hash_state_t){true, hash};
  pthread_mutex_unlock(&lock);

}
bool nr_cfg_epoch_note_sib1(uint32_t hash, uint64_t abs_slot)
{
  if (nr_cfg_ignore_sib1()) return false;
  if (!is_enabled()) return true;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (pending.active && abs_slot < pending.boundary) {
    pthread_mutex_unlock(&lock);
    return false; /* old-period SI cannot complete the requested comparison */
  }
  if (sib1.valid && sib1.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SIB1_CHANGE);
  sib1 = (hash_state_t){true, hash};
  pending.active = false;
  pthread_mutex_unlock(&lock);
  return true;
}
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t period)
{
  if (!is_enabled() || nr_cfg_ignore_sib1() || !period) return;
  pthread_mutex_lock(&lock);
  /* TS 38.331 5.2.2.2.2 defines SFN mod m = 0, not an arbitrary unwrapped
   * frame origin. For m > 1024 the only representable boundary SFN is zero. */
  const uint32_t sfn_cycle = 1024u * (slots_per_second / 100u);
  if (sfn_cycle && period > sfn_cycle) period = sfn_cycle;
  const uint64_t next_boundary = (abs_slot / period + 1) * (uint64_t)period;
  if (pending.active && pending.boundary == next_boundary) {
    pthread_mutex_unlock(&lock);
    return;
  }
  pending.active = true;
  pending.boundary = next_boundary;
  const uint64_t boundary = pending.boundary;
  pthread_mutex_unlock(&lock);
  LOG_I(PHY, "SENSING: CONFIG_EPOCH pending cause=SI_MODIFICATION_ANNOUNCED boundary=%lu\n",
        (unsigned long)boundary);
}
void nr_cfg_epoch_tick(uint64_t abs_slot)
{
  if (!is_enabled() || nr_cfg_ignore_sib1()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  const char *grace = getenv("ISAC_RECONF_SI_GRACE_MS");
  const unsigned grace_ms = grace && atoi(grace) > 0 ? (unsigned)atoi(grace) : 5000;
  if (pending.active && abs_slot >= pending.boundary
      && abs_slot - pending.boundary >= (uint64_t)grace_ms * slots_per_second / 1000) {
    const char *setting = getenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1");
    if (!setting || strcmp(setting, "0") != 0)
      bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
    pending.active = false;
  }
  pthread_mutex_unlock(&lock);

}
static void simple_bump(nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  bump_locked(&event, cls, cause);
  pthread_mutex_unlock(&lock);

}
void nr_cfg_epoch_note_continuity_loss(void) { simple_bump(NR_EPOCH_HARD_REVERIFY, NR_CAUSE_CONTINUITY_LOSS); }
void nr_cfg_epoch_note_continuity_loss_samples(int64_t gap_samples, uint64_t samples_per_ms)
{
  if (!is_enabled()) return;
  uint64_t threshold_ms = 10;
  const char *setting = getenv("ISAC_RECONF_GAP_HARD_MS");
  if (setting && *setting) {
    char *end;
    errno = 0;
    unsigned long long parsed = strtoull(setting, &end, 10);
    if (!errno && end != setting && !*end && setting[0] != '-') threshold_ms = parsed;
  }
  const bool hard = gap_samples <= 0 || !samples_per_ms ||
      (threshold_ms <= UINT64_MAX / samples_per_ms && (uint64_t)gap_samples >= threshold_ms * samples_per_ms);
  epoch_event_t event = {.has_gap = true, .gap_samples = gap_samples,
                         .samples_per_ms = samples_per_ms ? samples_per_ms : 1};
  pthread_mutex_lock(&lock);
  bump_locked(&event, hard ? NR_EPOCH_HARD_REVERIFY : NR_EPOCH_SOFT, NR_CAUSE_CONTINUITY_LOSS);
  pthread_mutex_unlock(&lock);
}
void nr_cfg_epoch_note_bwp_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_BWP_CHANGE); }
void nr_cfg_epoch_note_csirs_map_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_CSIRS_MAP_CHANGE); }
void nr_cfg_epoch_note_rnti_reopened(uint16_t rnti, bool was_converged, uint64_t abs_slot)
{
  if (!is_enabled() || !was_converged) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  const uint64_t window = 2u * (uint64_t)slots_per_second;
  if (reopened.valid && reopened.rnti != rnti && abs_slot >= reopened.slot && abs_slot - reopened.slot <= window) {
    bump_locked(&event, NR_EPOCH_SOFT, NR_CAUSE_DEDICATED_CHANGE_SUSPECTED);
    reopened.valid = false;
  } else if (!reopened.valid || reopened.rnti != rnti || abs_slot < reopened.slot || abs_slot - reopened.slot > window) {
    reopened = (reopened_t){true, rnti, abs_slot};
  }
  pthread_mutex_unlock(&lock);

}
