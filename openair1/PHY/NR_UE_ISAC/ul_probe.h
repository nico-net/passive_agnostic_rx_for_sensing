/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <cstdint>
#include <vector>

#include "pipeline_types.h"

/* Qualification-only observer of the UL differential batches (NR_ISAC_FIXED_WORK_REPLAY /
 * NR_ISAC_LIFECYCLE_TRACE builds only; absent from production builds).
 *
 * The harness uses it to dump, per CPI, the DL and UL measurement batches exactly as the tracker
 * receives them, so an offline replay can be compared block by block with a reference run. It must
 * never influence the pipeline: it takes everything by const reference and returns void.
 *
 * This header is the DEFAULT, harness-free definition: capture() is a no-op, so a diagnostic build
 * links and runs without the harness present. A qualification build replaces this translation unit.
 */
namespace ul_probe {

template <typename Config, typename Batches, typename UlBatches>
inline void capture(double /*midpoint_air_time_s*/,
                    std::uint64_t /*cpi_sequence*/,
                    const Config& /*config*/,
                    const Batches& /*batches*/,
                    const UlBatches& /*ul_batches*/)
{
}

/** Lifecycle-trace observers used by the multistatic tracker under NR_ISAC_LIFECYCLE_TRACE
 *  (multistatic_evidence.inc). All are no-ops here and take their arguments by const reference so
 *  the traced code path is identical to the untraced one. */
template <typename Evidence>
inline void validation(std::uint64_t /*sequence*/, std::uint64_t /*track_id*/,
                       std::uint64_t /*session*/, const Evidence& /*evidence*/)
{
}

template <typename State, typename Existence, typename Epoch>
inline void epoch(std::uint64_t /*track_id*/, const State& /*state*/,
                  const Existence& /*existence*/, const Epoch& /*epoch*/)
{
}

template <typename Family, typename State>
inline void family(std::uint64_t /*sequence*/, const Family& /*family*/,
                   std::uint64_t /*old_id*/, std::uint64_t /*new_id*/, const State& /*state*/)
{
}

template <typename Proposal>
inline void bootstrap(std::uint64_t /*sequence*/, std::uint64_t /*anchor_id*/,
                      std::uint64_t /*session*/, const Proposal& /*proposal*/,
                      bool /*has_causal_ue_state*/)
{
}

}  // namespace ul_probe
