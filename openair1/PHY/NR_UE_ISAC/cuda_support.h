/* SPDX-License-Identifier: OAI-Public-License-1.1 */
#pragma once

#include <cstdlib>

namespace nr_isac {

inline bool environment_flag_enabled(const char* name)
{
  const char* value = std::getenv(name);
  return value && std::atoi(value) != 0;
}

/** Fail closed instead of selecting any CPU sensing fallback. */
inline bool cuda_required()
{
  return environment_flag_enabled("NR_ISAC_REQUIRE_CUDA");
}

} // namespace nr_isac
