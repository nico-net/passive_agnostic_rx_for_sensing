/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this
 * file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/* Four-stage, bounded search. State belongs to the caller; serialize next/feed/reset. */
#ifndef NR_HYP_SWEEP_H
#define NR_HYP_SWEEP_H
#include <stdbool.h>
#include <stdint.h>
#define NR_HYP_SWEEP_MAX_RAW 8192
/* Sized from a MEASUREMENT, not a guess (2026-09-09). On the live cell -- UL BWP 0+273 and SIB1's
 * 6-entry pusch-TimeDomainAllocationList, i.e. a 3-bit TDA field, at the DCI 0_1 length 43 the
 * receiver locks automatically -- the generator emits 400 admissible width vectors, and against 8
 * real captured payloads they do NOT collapse to under 64 classes. At 64 the search returned
 * NR_HYP_SWEEP_CLASS_OVERFLOW and refused PERMANENTLY, which is why autonomous UL sat at zero
 * accepts while 19,094 format-0_1 DCIs per 480 s were being decoded correctly and handed to it.
 *
 * This does NOT relax the oracle, and it is not the "raise the caps to manufacture convergence"
 * that docs/ADAPTIVE_RX_VALIDATION_2026-09-09.md warns against: MIN_TRIALS, WIN_RATIO and MIN_RATE
 * are untouched, so nothing converges on weaker evidence than before. It only lets the search RUN
 * instead of declining to start. The cost it bounds is the CRC oracle's: every class must reach
 * MIN_TRIALS before any winner, so the price is MIN_TRIALS * n_classes transport blocks -- 512 x 300
 * = 153,600, about an hour at this cell's ~40 UL grants/s. The refusal path still exists above it. */
#define NR_HYP_SWEEP_MAX_CLASSES 512
#define NR_HYP_BYTES 96
#define NR_HYP_SWEEP_MIN_TRIALS 300
#define NR_HYP_SWEEP_WIN_RATIO 3.0
#define NR_HYP_SWEEP_MIN_RATE 0.02
#define NR_HYP_SWEEP_RAW_OVERFLOW (-1)
#define NR_HYP_SWEEP_CLASS_OVERFLOW (-2)
#define NR_HYP_SWEEP_INVALID (-3)
typedef struct { uint8_t bytes[NR_HYP_BYTES]; int len; } nr_hyp_t;
/* `skipped` counts candidates this class could not even interpret. A class that is never
 * plausible is never selected, so its `trials` stay 0 forever -- and the winner gate below
 * requires EVERY class to reach MIN_TRIALS, which made convergence unreachable. See
 * nr_hyp_sweep_feed(). */
typedef struct { nr_hyp_t hyp; uint64_t trials, passes, skipped; int members; } nr_hyp_class_t;
typedef struct {
  nr_hyp_class_t classes[NR_HYP_SWEEP_MAX_CLASSES];
  int n_classes, cursor, winner;
  /* Retain membership so callers can detect when new observations split a class. */
  int16_t class_of_raw[NR_HYP_SWEEP_MAX_RAW];
} nr_hyp_sweep_state_t;
typedef bool (*nr_hyp_constraint_fn)(const nr_hyp_t *, void *);
typedef bool (*nr_hyp_equivalent_fn)(const nr_hyp_t *, const nr_hyp_t *, const void *, void *);
typedef bool (*nr_hyp_plausible_fn)(const nr_hyp_t *, const void *, void *);
/* Callbacks may reject/partition only, never assign a quality score. samples is an array
 * of pointers, one per observation. Failure leaves an unusable, empty state, winner=-1. */
int nr_hyp_sweep_init(nr_hyp_sweep_state_t *, const nr_hyp_t *, int,
                     nr_hyp_constraint_fn, void *, nr_hyp_equivalent_fn,
                     const void *const *, int, void *);
int nr_hyp_sweep_next(nr_hyp_sweep_state_t *, const void *, nr_hyp_plausible_fn, void *, nr_hyp_t *);
int nr_hyp_sweep_feed(nr_hyp_sweep_state_t *, int, bool);
int nr_hyp_sweep_winner(const nr_hyp_sweep_state_t *);
#endif
