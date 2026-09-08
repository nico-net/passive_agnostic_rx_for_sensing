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
#define NR_HYP_SWEEP_MAX_CLASSES 64
#define NR_HYP_BYTES 96
#define NR_HYP_SWEEP_MIN_TRIALS 300
#define NR_HYP_SWEEP_WIN_RATIO 3.0
#define NR_HYP_SWEEP_MIN_RATE 0.02
#define NR_HYP_SWEEP_RAW_OVERFLOW (-1)
#define NR_HYP_SWEEP_CLASS_OVERFLOW (-2)
#define NR_HYP_SWEEP_INVALID (-3)
typedef struct { uint8_t bytes[NR_HYP_BYTES]; int len; } nr_hyp_t;
typedef struct { nr_hyp_t hyp; uint64_t trials, passes; int members; } nr_hyp_class_t;
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
