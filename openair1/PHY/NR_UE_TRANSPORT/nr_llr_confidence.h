#ifndef NR_LLR_CONFIDENCE_H
#define NR_LLR_CONFIDENCE_H
#include <stdint.h>
/* Decision-directed X-hat for CRC-failed grants (spec §6). The keep threshold is LEARNED from
 * CRC-OK grants: per Qm, the smallest min|LLR|/median|LLR| at which kept hard decisions are wrong
 * < NR_LLRCONF_TARGET_ERR of the time. Scale-free, because OAI renormalises LLRs per TB. */
#define NR_LLRCONF_BINS 80
#define NR_LLRCONF_BIN_W 0.05f
#define NR_LLRCONF_TARGET_ERR 0.01
#define NR_LLRCONF_MIN_SYMBOLS 100000ULL
/* Calibration (tau), the SNR gate and the disable latch are kept PER SOURCE: DL and UL SNR come from
 * different estimators, and a UL sign/packing fault must not disable DL. */
enum { NR_LLRCONF_SRC_DL, NR_LLRCONF_SRC_UL, NR_LLRCONF_NSRC };
void nr_llrconf_observe(int src, uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_threshold(int src, uint8_t qm, float *tau_rel);
uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau_rel, uint8_t *bits, uint8_t *keep);
void nr_llrconf_agreement(int src, uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_disabled(int src);
void nr_llrconf_stats_dump(void);
void nr_llrconf_reset(void);
float nr_llrconf_median_abs(const int16_t *llr, uint32_t G);
/* Byte-per-bit <-> packed LSB-first (the encoder's / nr_codeword_scrambling()'s format). */
void nr_llrconf_pack(const uint8_t *bits, uint32_t G, uint8_t *packed);
void nr_llrconf_unpack(const uint8_t *packed, uint32_t G, uint8_t *bits);
/* P34 grant-level gate: a CRC-failed grant may go masked only if its DM-RS SNR >= the learned p05
 * of CRC-OK grants' SNR at the same Qm (any consistent per-path scale). Off until the Qm has
 * NR_LLRCONF_MIN_SNR_GRANTS CRC-OK samples. */
/* 200 grants put ~10 samples below p05: a stable quantile in ~1 s of traffic, not ~10 min. */
#define NR_LLRCONF_MIN_SNR_GRANTS 200
#define NR_LLRCONF_SNR_QUANTILE 0.05
void nr_llrconf_snr_observe(int src, uint8_t qm, float snr_db);
int nr_llrconf_snr_eligible(int src, uint8_t qm, float snr_db);
/* Spec §6 per-source counters, bumped by the submitters only on an actual submission. */
enum { NR_LLRCONF_CNT_CRC_OK, NR_LLRCONF_CNT_MASKED, NR_LLRCONF_CNT_SNR_REJECT, NR_LLRCONF_CNT_RE_KEPT,
       NR_LLRCONF_CNT_RE_OFFERED, NR_LLRCONF_NCNT };
void nr_llrconf_count(int src, int what, uint64_t n);
uint64_t nr_llrconf_counter(int src, int what);
#endif
