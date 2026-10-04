/* Bounded, same-build passive IQ replay. No device access in the replay path. */
#ifndef NR_PASSIVE_REPLAY_CAPTURE_H
#define NR_PASSIVE_REPLAY_CAPTURE_H
#include "nr_dci_bits.h"
#include "nr_pdsch_passive_queue.h"
void nr_passive_replay_init(PHY_VARS_NR_UE *ue);
void nr_passive_replay_slot(PHY_VARS_NR_UE *ue, const UE_nr_rxtx_proc_t *proc);
void nr_passive_replay_ul(long source, uint16_t rnti, unsigned length, nr_dci_bits_t payload);
void nr_passive_replay_dl(const nr_pdsch_passive_job_t *job,
                          const nr_pdsch_passive_decode_result_t *result);
int nr_passive_replay_read(PHY_VARS_NR_UE *ue, const char *path);
#endif
