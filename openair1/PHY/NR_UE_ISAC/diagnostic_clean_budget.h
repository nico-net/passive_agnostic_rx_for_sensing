/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

/* Fixed-work replay harness hook (NR_ISAC_FIXED_WORK_REPLAY builds only).
 *
 * The offline qualification harness bounds the CLEAN component budget per (CPI, receiver) so that
 * a replay performs exactly the same amount of work on every host, independently of the machine's
 * speed: the production detector stops on its own next-CPI processing deadline, which is wall-clock
 * dependent and therefore not reproducible across machines.
 *
 * This header is the DEFAULT, harness-free definition: no limit is imposed and select() is a no-op,
 * so a NR_ISAC_FIXED_WORK_REPLAY build behaves exactly like production except for the deadline
 * reset that clean_detector.cc performs unconditionally under that macro. A qualification build
 * replaces this translation unit with one that records a per-(sequence, receiver) budget.
 *
 * component_limit semantics, as read by clean_detector.cc:
 *     -2  no diagnostic context has been selected -> the detector throws (harness misuse)
 *     -1  no limit (this default)
 *   >= 1  stop CLEAN once that many components have been accepted
 */
namespace diagnostic_clean {

inline thread_local int component_limit = -1;

/** Select the diagnostic context for the (CPI sequence, receiver) about to be processed.
 *  `uplink` distinguishes the UL detector pass from the DL one on the same receiver.
 *  The default implementation keeps `component_limit` at "no limit". */
inline void select(unsigned long long /*sequence*/, unsigned /*receiver*/, bool /*uplink*/)
{
  component_limit = -1;
}

}  // namespace diagnostic_clean
