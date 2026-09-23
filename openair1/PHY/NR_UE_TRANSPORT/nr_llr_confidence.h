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
void nr_llrconf_observe(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_threshold(uint8_t qm, float *tau_rel);
uint32_t nr_llrconf_hard(uint8_t qm, const int16_t *llr, uint32_t G, float tau_rel, uint8_t *bits, uint8_t *keep);
void nr_llrconf_agreement(uint8_t qm, const int16_t *llr, const uint8_t *truth_bits, uint32_t G);
int nr_llrconf_disabled(void);
void nr_llrconf_stats_dump(void);
void nr_llrconf_reset(void);
float nr_llrconf_median_abs(const int16_t *llr, uint32_t G);
#endif
