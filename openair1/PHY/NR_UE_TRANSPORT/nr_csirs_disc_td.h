/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_TD_H
#define NR_CSIRS_DISC_TD_H
#include "nr_csirs_disc.h"
#include "PHY/defs_nr_UE.h"

/* Actor copies the contiguous CP-bearing span read by nr_slot_fep, with its
 * capture-time CFO, CP offsets and symbol rotations. No FFT or allocation. */
int nr_csirs_disc_td_push(PHY_VARS_NR_UE *ue, uint32_t abs_slot, uint16_t frame, uint8_t slot);
int nr_csirs_disc_td_init(const NR_DL_FRAME_PARMS *fp, int n_ant, int capacity);
void nr_csirs_disc_td_free(void);
void nr_csirs_disc_td_reset(void);
int nr_csirs_disc_td_pending(void);
int nr_csirs_disc_td_drain(nr_csirs_disc_ring_t *ring);
/* CPU reference used by the worker and parity fixture: packed FFT windows. */
void nr_csirs_disc_fft_cpu(int fft, int transforms, const int16_t *in, int16_t *out);
extern uint64_t csirs_disc_fep_gpu_chunks, csirs_disc_fep_cpu_chunks;
extern uint64_t csirs_disc_fep_us, csirs_disc_fep_max_us;
extern uint64_t csirs_disc_fft_us, csirs_disc_fft_batches, csirs_disc_td_overflows;
#endif
