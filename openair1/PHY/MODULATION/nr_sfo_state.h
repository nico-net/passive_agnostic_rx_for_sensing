/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Receiver sampling-frequency offset (SFO): the one eps provider of the receiver, and the FEP-side correction of the timing drift it
 * causes within a slot (DL and UL).
 *
 * WHY. The receiver reads the radio with its own sample clock. A clock offset eps (ppm scale; the X410 and a gNB keep carrier and
 * sample clock on one reference, so eps ~ -CFO / fc) moves the FFT window by eps * n samples over n samples. The timing loop repairs
 * the slot-to-slot part (it reads more or fewer samples once per frame), but inside a slot symbol l is still eps * n_l samples off
 * against symbol 0: subcarrier k turns by 2 pi k eps n_l / N. The DM-RS estimate of one symbol carries the rotation of that symbol only,
 * so data symbols away from it see a phase error growing with |k|: nothing at DC, 0.3 rad at the edge of a 100 MHz carrier between
 * symbols 2 and 11 at 4 ppm. A narrow grant sees it as a common phase; a wide 256QAM grant fails in the code blocks far from DC
 * [MEASURED, sens6 X410 OTA lab cell 2026-10-25: raw DM-RS symbol-difference SNR 33.9 dB for narrow grants, 10-15 dB at the band edges
 * of 273-PRB grants, 29 dB next to DC].
 *
 * SIGN. eps > 0: the receiver clock is FAST (it takes (1 + eps) fs samples per transmitter second). The timing loop then reads MORE than
 * samples_per_frame samples per frame on average: nr_sfo_note_frame_read(extra) receives extra = samples read beyond nominal in the
 * frame (= -iq_shift_to_apply in nr-ue.c), and eps = mean(extra) / samples_per_frame. The FEP multiplies subcarrier k of symbol l by
 * exp(+j 2 pi k eps n_l / N), n_l = nr_sfo_symbol_offset() (samples from the start of symbol 0's window to symbol l's, CP included).
 *
 * SOURCES (device measurements only, nothing from the command line or the host): (1) the timing loop's own per-frame read length, a
 * windowed mean (NR_SFO_WINDOW frames); (2) a cross-check from the DM-RS: the per-grant phase slope across subcarriers between the first
 * and last DM-RS symbol of a corrected slot is the RESIDUAL eps, fed back with a small gain (nr_sfo_note_dmrs_residual). Published
 * value: nr_sfo_eps_q() (lock-free), in units of 1e-12, changed only when it moves by more than NR_SFO_HYST_Q so that a CPU and a GPU
 * FEP of the same slot use the same value. No eps (0) = every FEP is bit-identical to the uncorrected one (rfsim, simulators, gNB). */
#ifndef NR_SFO_STATE_H
#define NR_SFO_STATE_H
#include <stdint.h>
#include "nr_sfo_rot.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NR_SFO_WINDOW 256          /* frames of the timing-loop mean (2.56 s) */
#define NR_SFO_HYST_Q 20000        /* 0.02 ppm in 1e-12 units: below this a change is not published */
#define NR_SFO_MAX_PPM 100.0       /* beyond any real oscillator: a larger measurement is a timing jump, not an offset */

void nr_sfo_reset(void);
/* timing loop, once per frame: samples read beyond samples_per_frame (negative = fewer) */
void nr_sfo_note_frame_read(int extra_samples, int samples_per_frame);
/* DM-RS cross-check: the residual eps measured on an already corrected slot (weight in [0,1], e.g. grant coherence) */
void nr_sfo_note_dmrs_residual(double eps_residual, double weight);
/* published eps, 1e-12 units (lock-free) */
int64_t nr_sfo_eps_q(void);
double nr_sfo_eps(void);
/* thread-local pin of the value a FEP uses (a GPU job and its CPU self-check of the same slot); INT64_MIN = none */
void nr_sfo_pin(int64_t eps_q);
int64_t nr_sfo_eps_q_for_fep(void);
/* samples from the start of symbol 0's FFT window to symbol l's in this slot (CPs included) */
int nr_sfo_symbol_offset(int ofdm_symbol_size, int nb_prefix_samples, int nb_prefix_samples0, int numerology, int symbols_per_slot, int slot,
                         int symbol);
/* phase step per subcarrier, 2^-32 turns: 2 pi eps n_l / N */
uint32_t nr_sfo_dphi(int64_t eps_q, int ofdm_symbol_size, int n_l);
/* the per-symbol steps of one slot, out[l] = nr_sfo_dphi(eps_q, N, n_l) for l < symbols_per_slot (<= 14); returns 0 when eps_q is 0
 * (every step 0), else 1. What a GPU FEP job carries so that its output equals the CPU FEP of the same slot. */
int nr_sfo_slot_dphi(int64_t eps_q, int ofdm_symbol_size, int nb_prefix_samples, int nb_prefix_samples0, int numerology, int symbols_per_slot,
                     int slot, uint32_t out[14]);
/* correct one frequency-domain symbol (N bins, OAI layout: positive frequencies from bin 0, negative from first_carrier_offset) on the
 * occupied bins of nb_rb PRBs, exactly as the rotation step addresses them; dphi 0 = no-op */
void nr_sfo_rotate_symbol(int16_t *symb, int N, int first_carrier_offset, int nb_rb, uint32_t dphi);

#ifdef __cplusplus
}
#endif
#endif
