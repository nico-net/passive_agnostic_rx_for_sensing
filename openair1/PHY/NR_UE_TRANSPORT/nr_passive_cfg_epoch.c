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
static _Atomic uint64_t cfr_stamp;
static _Atomic uint32_t identity_gen;
static _Atomic uint32_t dedicated_epoch;
static _Atomic uint64_t current_slot;
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
static pthread_once_t ignore_sib1_once = PTHREAD_ONCE_INIT;
static bool ignore_sib1;
static pthread_once_t si_options_once = PTHREAD_ONCE_INIT;
static unsigned si_grace_ms;
static bool si_bump_without_sib1;

/* read_enable: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_RECONF.
 *   Callers: none found in tree.
 */
static void read_enable(void)
{
  const char *s = getenv("ISAC_RECONF");
  enabled = s && strcmp(s, "1") == 0;
}
/* is_enabled: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:79, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:207, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:277, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:302 (+9 more).
 */
static bool is_enabled(void)
{
  pthread_once(&enable_once, read_enable);
  return enabled;
}
/* nr_cfg_reconf_enabled: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue.c:1173, executables/nr-ue.c:1824, executables/nr-ue.c:1826, executables/nr-uesoftmodem.c:408 (+106 more).
 */
bool nr_cfg_reconf_enabled(void) { return is_enabled(); }
/* read_ignore_sib1: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_TD_IGNORE_SIB1.
 *   Callers: none found in tree.
 */
static void read_ignore_sib1(void)
{
  const char *value = getenv("ISAC_TD_IGNORE_SIB1");
  ignore_sib1 = value && strcmp(value, "1") == 0;
}
/* nr_cfg_ignore_sib1: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:276, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:281, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:286, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:294 (+25 more).
 */
bool nr_cfg_ignore_sib1(void)
{
  pthread_once(&ignore_sib1_once, read_ignore_sib1);
  return ignore_sib1;
}
/* nr_cfg_ignore_sib1_reset_for_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:68, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:26, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:109, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:128 (+14 more).
 */
void nr_cfg_ignore_sib1_reset_for_test(void)
{
  /* Test harnesses call this only between receiver operations, never concurrently. */
  ignore_sib1_once = PTHREAD_ONCE_INIT;
  ignore_sib1 = false;
}
/* read_si_options: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_RECONF_SI_BUMP_WITHOUT_SIB1, ISAC_RECONF_SI_GRACE_MS.
 *   Callers: none found in tree.
 */
static void read_si_options(void)
{
  const char *grace = getenv("ISAC_RECONF_SI_GRACE_MS");
  si_grace_ms = grace && atoi(grace) > 0 ? (unsigned)atoi(grace) : 5000;
  const char *setting = getenv("ISAC_RECONF_SI_BUMP_WITHOUT_SIB1");
  si_bump_without_sib1 = !setting || strcmp(setting, "0") != 0;
}
/* nr_cfg_si_options_reset_for_test: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:69, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:37, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:168, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:175 (+2 more).
 */
void nr_cfg_si_options_reset_for_test(void)
{
  /* Test harnesses call this only between receiver operations. */
  si_options_once = PTHREAD_ONCE_INIT;
}
/* class_name: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:162.
 */
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
/* bump_locked: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:334, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:351, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:362, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:377 (+4 more).
 */
static void bump_locked(epoch_event_t *event, nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  state.epoch++;
  if (cls == NR_EPOCH_HARD_RESET) state.identity_gen++;
  state.last_class = cls;
  state.last_cause = cause;
  if (cls != NR_EPOCH_SOFT || cause == NR_CAUSE_DEDICATED_CHANGE_SUSPECTED)
    state.dedicated_epoch = state.epoch;
  atomic_store_explicit(&dedicated_epoch, state.dedicated_epoch, memory_order_release);
  atomic_store_explicit(&epoch, state.epoch, memory_order_release);
  atomic_store_explicit(&identity_gen, state.identity_gen, memory_order_release);
  atomic_store_explicit(&cfr_stamp, ((uint64_t)state.epoch << 32) | state.identity_gen, memory_order_release);
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
/* dispatch: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:186.
 */
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
/* nr_cfg_epoch_drain: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:88, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4497, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:69, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:71 (+56 more).
 */
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

/* nr_cfg_epoch_work_begin: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:57, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:63, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:374, openair1/PHY/NR_UE_TRANSPORT/tests/nr_reconfig_replay_test.cc:207.
 */
void nr_cfg_epoch_work_begin(nr_cfg_epoch_work_t *work, uint32_t stamp, void *counter)
{
  *work = (nr_cfg_epoch_work_t){.parent = current_work, .epoch = stamp, .dropped_counter = counter};
  work->owner = current_work && current_work->epoch == stamp
      && (!counter || counter == current_work->owner->dropped_counter) ? current_work->owner : work;
  current_work = work;
}
/* nr_cfg_epoch_work_end: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:58, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:380, openair1/PHY/NR_UE_TRANSPORT/tests/nr_reconfig_replay_test.cc:209.
 */
void nr_cfg_epoch_work_end(nr_cfg_epoch_work_t *work) { current_work = work->parent; }
/* nr_cfg_epoch_work_stamp: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c:75, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:59, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4498, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4562 (+8 more).
 */
uint32_t nr_cfg_epoch_work_stamp(void)
{
  return current_work ? current_work->epoch : nr_cfg_epoch_current();
}
/* nr_cfg_epoch_work_current: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c:35, openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c:72, openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:69, openair1/PHY/NR_UE_TRANSPORT/nr_dci_history.c:73 (+113 more).
 */
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

/* nr_cfg_epoch_current: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:203, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:207, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:65, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.c:1055 (+49 more).
 */
uint32_t nr_cfg_epoch_current(void) { return atomic_load_explicit(&epoch, memory_order_acquire); }
/* nr_cfg_epoch_identity_gen: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:70, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.c:209, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.c:395, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1821 (+5 more).
 */
uint32_t nr_cfg_epoch_identity_gen(void) { return atomic_load_explicit(&identity_gen, memory_order_acquire); }
/* nr_cfg_epoch_dedicated_current: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_ISAC/nr_isac.cc:360, openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c:42, openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.c:94, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:46 (+6 more).
 */
uint32_t nr_cfg_epoch_dedicated_current(void) { return atomic_load_explicit(&dedicated_epoch, memory_order_acquire); }
/* nr_cfg_epoch_current_slot: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:47, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:2136, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:116.
 */
uint64_t nr_cfg_epoch_current_slot(void) { return atomic_load_explicit(&current_slot, memory_order_acquire); }
/* nr_cfg_epoch_cfr_stamp: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_ISAC/nr_isac.cc:359, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:73, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_passive_queue.c:2331, openair1/PHY/NR_UE_TRANSPORT/nr_prach_passive.c:619 (+8 more).
 */
uint64_t nr_cfg_epoch_cfr_stamp(void) { return atomic_load_explicit(&cfr_stamp, memory_order_acquire); }
/* nr_cfg_epoch_snapshot: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:71, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_coreset_bank.c:95, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_dci_length_sweep.c:713, openair1/PHY/NR_UE_TRANSPORT/nr_pdsch_config_sweep.c:1813 (+4 more).
 */
nr_cfg_epoch_snapshot_t nr_cfg_epoch_snapshot(void)
{
  pthread_mutex_lock(&lock);
  nr_cfg_epoch_snapshot_t result = state;
  pthread_mutex_unlock(&lock);
  return result;
}
/* nr_cfg_epoch_reset: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:102, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:195, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:209, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci_history_test.cc:504 (+34 more).
 */
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
  atomic_store_explicit(&cfr_stamp, 0, memory_order_release);
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
  atomic_store_explicit(&dedicated_epoch, 0, memory_order_release);
  atomic_store_explicit(&current_slot, 0, memory_order_release);
  atomic_store_explicit(&epoch, 0, memory_order_release);
  atomic_store_explicit(&identity_gen, 0, memory_order_release);
  pthread_mutex_unlock(&lock);
}
/* nr_cfg_epoch_subscribe: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-uesoftmodem.c:414, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:17, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:147, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_disc.c:182 (+8 more).
 */
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
/* nr_cfg_epoch_set_slots_per_second: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue.c:1182, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:94, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:384, openair1/PHY/NR_UE_TRANSPORT/tests/nr_reconfig_replay_test.cc:133.
 */
void nr_cfg_epoch_set_slots_per_second(uint32_t value)
{
  if (!value) return;
  pthread_mutex_lock(&lock);
  slots_per_second = value;
  reopened.valid = false;
  pthread_mutex_unlock(&lock);
}
/* nr_cfg_epoch_set_si_period: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:95, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:113, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:346, openair2/LAYER2/NR_MAC_UE/config_ue.c:2589.
 */
void nr_cfg_epoch_set_si_period(uint32_t value)
{
  if (nr_cfg_ignore_sib1()) return;
  if (is_enabled()) atomic_store_explicit(&si_period_slots, value, memory_order_release);
}
/* nr_cfg_epoch_si_period: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:96, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8216, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8250, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:119 (+2 more).
 */
uint32_t nr_cfg_epoch_si_period(void)
{
  if (nr_cfg_ignore_sib1()) return 0;
  return atomic_load_explicit(&si_period_slots, memory_order_acquire);
}
/* nr_cfg_epoch_si_boundary: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:97, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:120, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:386, openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c:629.
 */
uint64_t nr_cfg_epoch_si_boundary(void)
{
  if (nr_cfg_ignore_sib1()) return 0;
  pthread_mutex_lock(&lock);
  const uint64_t boundary = pending.active ? pending.boundary : 0;
  pthread_mutex_unlock(&lock);
  return boundary;
}
/* nr_cfg_epoch_sib1_request_allowed: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:98, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:121, openair2/LAYER2/NR_MAC_UE/nr_ue_dci_configuration.c:631.
 */
bool nr_cfg_epoch_sib1_request_allowed(uint64_t abs_slot)
{
  if (nr_cfg_ignore_sib1()) return false;
  pthread_mutex_lock(&lock);
  const bool allowed = !pending.active || abs_slot >= pending.boundary;
  pthread_mutex_unlock(&lock);
  return allowed;
}
/* nr_cfg_epoch_si_redecode_pending: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:99, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:122, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:323, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:356 (+1 more).
 */
bool nr_cfg_epoch_si_redecode_pending(uint64_t abs_slot)
{
  if (!is_enabled() || nr_cfg_ignore_sib1()) return false;
  pthread_mutex_lock(&lock);
  const bool needed = pending.active && abs_slot >= pending.boundary;
  pthread_mutex_unlock(&lock);
  return needed;
}
/* nr_cfg_epoch_observe_slot: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:100, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4474, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4957, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:448 (+4 more).
 */
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
/* nr_cfg_epoch_note_identity: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: executables/nr-ue.c:1180, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:74, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:40, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:48 (+17 more).
 */
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
/* nr_cfg_epoch_refine_point_a: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:75, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:117, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:432, openair2/LAYER2/NR_MAC_UE/config_ue.c:2582.
 */
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
/* nr_cfg_epoch_note_mib: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:76, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:84, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:85, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:123 (+3 more).
 */
void nr_cfg_epoch_note_mib(uint32_t hash)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (mib.valid && mib.hash != hash) bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_MIB_CHANGE);
  mib = (hash_state_t){true, hash};
  pthread_mutex_unlock(&lock);

}
/* nr_cfg_epoch_note_sib1: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:77, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:93, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:94, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:101 (+21 more).
 */
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
/* nr_cfg_epoch_note_si_modification: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:78, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8216, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8250, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:114 (+10 more).
 */
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
/* nr_cfg_epoch_tick: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:87, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:4958, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:115, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:137 (+13 more).
 */
void nr_cfg_epoch_tick(uint64_t abs_slot)
{
  if (!is_enabled()) return;
  atomic_store_explicit(&current_slot, abs_slot, memory_order_release);
  if (nr_cfg_ignore_sib1()) return; /* SIB1-less arm: no SI-modification boundary handling */
  pthread_once(&si_options_once, read_si_options);
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  if (pending.active && abs_slot >= pending.boundary
      && abs_slot - pending.boundary >= (uint64_t)si_grace_ms * slots_per_second / 1000) {
    if (si_bump_without_sib1)
      bump_locked(&event, NR_EPOCH_HARD_REVERIFY, NR_CAUSE_SI_MODIFICATION_ANNOUNCED);
    pending.active = false;
  }
  pthread_mutex_unlock(&lock);

}
/* simple_bump: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:429, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:431, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:451, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.c:452.
 */
static void simple_bump(nr_epoch_class_t cls, nr_epoch_cause_t cause)
{
  if (!is_enabled()) return;
  epoch_event_t event = {0};
  pthread_mutex_lock(&lock);
  bump_locked(&event, cls, cause);
  pthread_mutex_unlock(&lock);

}
/* nr_cfg_epoch_note_continuity_loss: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:79, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:220, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:337, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:443 (+7 more).
 */
void nr_cfg_epoch_note_continuity_loss(void) { simple_bump(NR_EPOCH_HARD_REVERIFY, NR_CAUSE_CONTINUITY_LOSS); }
/* nr_cfg_epoch_note_rx_recovery: records a soft configuration epoch caused by an RX recovery (the samples were restarted, the configuration is kept). Caller: executables/nr-ue.c (RX recovery after a stream restart). */
void nr_cfg_epoch_note_rx_recovery(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_RX_RECOVERY); }
/* nr_cfg_epoch_note_continuity_loss_samples: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: ISAC_RECONF_GAP_HARD_MS.
 *   Callers: executables/nr-ue.c:1830, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:83, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:214, openair1/PHY/NR_UE_TRANSPORT/tests/nr_passive_cfg_epoch_test.cc:193 (+8 more).
 */
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
/* nr_cfg_epoch_note_bwp_change: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:84, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:8279, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:216, openair1/PHY/NR_UE_TRANSPORT/tests/nr_dci11_pin_test.cc:221 (+20 more).
 */
void nr_cfg_epoch_note_bwp_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_BWP_CHANGE); }
/* nr_cfg_epoch_note_csirs_map_change: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:42, openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_rt.c:102, openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:85, openair1/PHY/NR_UE_TRANSPORT/tests/nr_csirs_blind_rt_test_bridge.c:469 (+6 more).
 */
void nr_cfg_epoch_note_csirs_map_change(void) { simple_bump(NR_EPOCH_SOFT, NR_CAUSE_CSIRS_MAP_CHANGE); }
/* nr_cfg_epoch_note_rnti_reopened: TODO(doc).
 *   How it works: TODO(doc).
 *   ISAC_* flags read: none.
 *   Callers: openair1/PHY/NR_UE_TRANSPORT/nr_passive_cfg_epoch.h:86, openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c:50, openair1/PHY/NR_UE_TRANSPORT/nr_passive_ue_ctx.c:191, openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.c:5909 (+20 more).
 */
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
