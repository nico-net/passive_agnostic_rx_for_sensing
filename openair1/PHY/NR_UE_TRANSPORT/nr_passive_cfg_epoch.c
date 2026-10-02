#include "nr_passive_cfg_epoch.h"
#include "common/utils/LOG/log.h"
#include <pthread.h>
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
  bool active, observed;
  uint64_t boundary;
  uint32_t candidate;
} si_pending_t;
typedef struct {
  bool valid;
  uint16_t rnti;
  uint64_t slot;
} reopened_t;
typedef struct {
  bool bumped;
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
}
static void dispatch(const epoch_event_t *event)
{
  if (!event->bumped) return;
  const nr_cfg_epoch_snapshot_t *s = &event->snapshot;
  LOG_I(PHY, "SENSING: CONFIG_EPOCH %u -> %u class=%s cause=%s scope=%s\n",
        s->epoch - 1, s->epoch, class_name(s->last_class), cause_name(s->last_cause),
        s->last_class == NR_EPOCH_HARD_RESET ? "cell_identity" : "cell");
  for (unsigned i = 0; i < event->n_listeners; ++i) event->listeners[i](s);
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
  if (is_enabled()) atomic_store_explicit(&si_period_slots, value, memory_order_release);
}
uint32_t nr_cfg_epoch_si_period(void)
{
  return atomic_load_explicit(&si_period_slots, memory_order_acquire);
}
bool nr_cfg_epoch_si_redecode_pending(void)
{
  if (!is_enabled()) return false;
  pthread_mutex_lock(&lock);
  const bool needed = pending.active && !pending.observed;
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
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  identity = (cell_identity_t){true, pci, ssb_arfcn, point_a ? point_a : (changed ? 0 : identity.point_a)};
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
void nr_cfg_epoch_refine_point_a(uint64_t point_a)
{
  if (!is_enabled() || !point_a) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (identity.valid && identity.point_a && identity.point_a != point_a) {
    memset(&mib, 0, sizeof(mib));
    memset(&sib1, 0, sizeof(sib1));
    memset(&pending, 0, sizeof(pending));
    reopened.valid = false;
    bump_locked(&event, NR_EPOCH_HARD_RESET, NR_CAUSE_CELL_IDENTITY_CHANGE);
  }
  if (identity.valid) identity.point_a = point_a;
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
void nr_cfg_epoch_note_mib(uint32_t hash)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (mib.valid && mib.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_MIB_CHANGE);
  mib = (hash_state_t){true, hash};
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
void nr_cfg_epoch_note_sib1(uint32_t hash)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (pending.active) {
    pending.observed = true;
    pending.candidate = hash;
  } else {
    if (sib1.valid && sib1.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SIB1_CHANGE);
    sib1 = (hash_state_t){true, hash};
  }
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
void nr_cfg_epoch_note_si_modification(uint64_t abs_slot, uint32_t period)
{
  if (!is_enabled() || !period) return;
  pthread_mutex_lock(&lock);
  const uint64_t next_boundary = (abs_slot / period + 1) * (uint64_t)period;
  if (pending.active && pending.boundary == next_boundary) {
    pthread_mutex_unlock(&lock);
    return;
  }
  pending.active = true;
  pending.observed = false;
  pending.boundary = next_boundary;
  const uint64_t boundary = pending.boundary;
  pthread_mutex_unlock(&lock);
  LOG_I(PHY, "SENSING: CONFIG_EPOCH pending cause=SI_MODIFICATION_ANNOUNCED boundary=%lu\n",
        (unsigned long)boundary);
}
void nr_cfg_epoch_tick(uint64_t abs_slot)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (pending.active && abs_slot >= pending.boundary) {
    if (pending.observed) {
      if (!sib1.valid || pending.candidate != sib1.hash)
        bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SIB1_CHANGE);
      sib1 = (hash_state_t){true, pending.candidate};
    } else {
      const char *setting = getenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1");
      if (!setting || strcmp(setting, "0") != 0)
        bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
    }
    pending.active = false;
  }
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
static void simple_bump(nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  bump_locked(&event, cls, cause);
  pthread_mutex_unlock(&lock);
  dispatch(&event);
}
void nr_cfg_epoch_note_continuity_loss(void) { simple_bump(NR_EPOCH_HARD_REVERIFY, NR_CAUSE_CONTINUITY_LOSS); }
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
  dispatch(&event);
}
