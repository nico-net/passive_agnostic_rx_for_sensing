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
 */

/*! \file openair1/PHY/NR_UE_TRANSPORT/nr_dci11_pin.h
 * \brief R30 item 2 / R32 fix round 1 (technique-d-regression.md, td-fix-report.md): the pure
 *        pin/rotate/giveup decision for Technique D's per-RNTI DCI-11 layout-candidate selection
 *        (nr_pdcch_blind_monitor_rt.c). Extracted here, dependency-free, so it is unit-testable
 *        without linking the RT scan-thread code or the sweep module -- see
 *        tests/nr_dci11_pin_test.cc.
 *
 * THREADING (confirmed by reading both call sites, not assumed): the caller,
 * nr_pdcch_blind_monitor_run_occasion(), is invoked from BOTH the live RT receive thread
 * (nr_pdcch_blind_monitor_rt.c: "on the RT thread: fan out as before") and the deferred queue
 * consumer thread (nr_pdcch_passive_queue.c: "already off the RT thread"). The SAME RNTI's
 * occasions can therefore reach this code from two different threads. `valid` is the only field
 * whose cross-thread visibility matters for correctness (it gates whether `layout`/`cfg`/`occ` are
 * meaningful at all), so it is published with a release store after they are written, and observed
 * with an acquire load before they are read -- the standard "flag publishes a record" idiom.
 * `occ`'s own increments are NOT further synchronized: an occasional lost or double count only
 * shifts a rotation boundary by about one occasion, which is inside this mechanism's own noise (it
 * exists to bound exploration, not to count exactly) and not a correctness issue worth a lock on
 * this path. Two threads racing to independently reseed the SAME invalid pin can likewise leave a
 * torn (mismatched layout/cfg) record for one occasion; the next occasion's cfg check or rotation
 * bound recovers it. Accepted for the same reason.
 *
 * `valid` is declared plain `bool` here (not `_Atomic bool`) so this struct has one unambiguous
 * layout in both the C production code and the C++ test binary that links against the same
 * nr_dci11_pin.c object file -- C11's `_Atomic bool` and C++11's `std::atomic<bool>` are widely
 * relied upon to be layout-compatible in practice, but this file would rather not depend on that.
 * nr_dci11_pin.c (always compiled as C) accesses `valid` through an `_Atomic bool *` cast for its
 * two release/acquire operations instead, which is standard C11 (atomic-qualifying a pointer to an
 * already-compatible plain object is exactly what "atomic operations on non-atomic objects via a
 * pointer" supports) and keeps the header itself free of any atomic type.
 */

#ifndef __NR_DCI11_PIN_H__
#define __NR_DCI11_PIN_H__

#include <stdint.h>
#include <stdbool.h>

typedef struct {
  bool     valid; /* see the file header: touched only via an _Atomic bool* cast in nr_dci11_pin.c */
  uint16_t layout;
  uint64_t cfg;
  uint32_t occ;
} nr_dci11_pin_t;

/** One occasion's decision. `layout_ids`/`n` are this occasion's offered DCI-11 layout candidates.
 * `settled`/`preferred` (>=0) bypass the pin entirely and are returned unchanged, pin untouched --
 * same precedence as the caller's own pre-existing evidence-based promotions.
 * `has_stats`/`trial_ok`/`trial_tr` are the CALLER's own lookup (e.g.
 * nr_pdsch_config_sweep_context_stats) of the CURRENTLY PINNED candidate's real Technique-D trial
 * history; pass has_stats=false when there was no valid pin to look one up for.
 * Returns the index into layout_ids[0..n) to use this occasion, or -1 when the caller must pick a
 * new candidate itself (any policy -- see nr_dci11_pin_round_robin() for the plain one) and commit
 * it via nr_dci11_pin_seed(). After a -1 return, check pin->valid: if it is now false, the pin was
 * genuinely dropped (rotated out on block_occasions/giveup_trials, a real cfg change, or it was
 * never seeded) and MUST be reseeded; if it is still true, the pin is merely absent from this
 * occasion's offered list (transient) and must NOT be reseeded, so it can resume the moment it
 * reappears. */
int nr_dci11_pin_select(nr_dci11_pin_t *pin, uint64_t current_cfg, const uint16_t *layout_ids, int n,
                        int settled, int preferred, bool has_stats, uint32_t trial_ok, uint32_t trial_tr,
                        uint32_t block_occasions, uint32_t giveup_trials);

/** Commits a freshly chosen candidate as the pin. Only called by the caller after a -1 return with
 * pin->valid == false (see nr_dci11_pin_select()'s contract). */
void nr_dci11_pin_seed(nr_dci11_pin_t *pin, uint64_t current_cfg, uint16_t layout_id);

/** Plain round-robin picker for seeding/rotating a pin when no informed signal (e.g. Thompson
 * evidence) exists. Advances *cursor by exactly one candidate per CALL, so n successive calls with
 * the same cursor visit every one of n candidates exactly once, regardless of how many OCCASIONS
 * separate the calls -- the bug this fixes (R32): the original caller advanced an equivalent
 * cursor every OCCASION but only read it every ~block_occasions occasions, so successive picks
 * landed gcd(block_occasions, n) apart instead of 1 apart, starving most candidates whenever
 * gcd(block_occasions, n) > 1 (e.g. n=8 with block=50: only 2 of 8 ever pinned). The caller must
 * therefore only call this at the point of actually reseeding, never on every occasion. */
int nr_dci11_pin_round_robin(uint32_t *cursor, int n);

#endif
