#pragma once
/* ISAC_PDSCH_RETRY_YIELD=2 (P99): rescue-rate gate of one DECODE-FIX recovery arm, the same rule for every arm and cell.
 * An arm is always tried until it has NR_RETRY_GATE_MIN_TRY tries; after that only while it rescued >= 1 in
 * NR_RETRY_GATE_MIN_RATE of them, otherwise on 1 in NR_RETRY_GATE_EXPLORE chains (seen = this arm's gate calls so far),
 * so an arm that starts to work again is found. Pure function; the caller keeps the counters. */
#include <stdbool.h>
#include <stdint.h>
#define NR_RETRY_GATE_MIN_TRY 256
#define NR_RETRY_GATE_MIN_RATE 50
#define NR_RETRY_GATE_EXPLORE 16
static inline bool nr_retry_gate_pass(uint64_t tries, uint64_t rescues, uint64_t seen)
{
  return tries < NR_RETRY_GATE_MIN_TRY || rescues * NR_RETRY_GATE_MIN_RATE >= tries || seen % NR_RETRY_GATE_EXPLORE == 0;
}
