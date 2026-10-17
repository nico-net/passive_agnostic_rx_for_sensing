/* Pure helpers of the UL n_ID background search (nr_ul_nid_bg.h); no dependencies. */
#ifndef NR_UL_NID_RULE_H
#define NR_UL_NID_RULE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ---- pure helpers (unit-tested) ---- */
/* FDRA type-1 bit width of a BWP of n PRB: ceil(log2(n (n+1) / 2)) */
int nr_ul_nid_riv_bits(int n);
/* TS 38.214 6.1.2.2.2 decode of a RIV under BWP size n. true and *s, *l when it is a valid (start, length). */
bool nr_ul_nid_riv_decode(uint32_t riv, int n, int *s, int *l);
/* Twin readings of one RIV: every BWP size n' with |n' - n| <= span and the same FDRA width as n whose decode is valid; n itself first, then by
 * |n' - n|. Returns the count (<= cap). */
int nr_ul_nid_twins(uint32_t riv, int n, int span, uint16_t *on, uint16_t *os, uint16_t *ol, int cap);
/* Confirmation rule: a weak (CRC16) hit with `key` at abs_slot. state: up to 2 earlier slots of the same key. */
typedef struct {
  uint64_t key;
  int64_t slot[2];
  uint8_t n;
} nr_ul_nid_pend_t;
/* true = confirmed. strong (CRC24) confirms alone; a weak hit needs a second DIFFERENT slot of the same key. */
bool nr_ul_nid_confirm_rule(nr_ul_nid_pend_t *p, bool strong, int64_t abs_slot);
#ifdef __cplusplus
}
#endif
#endif
