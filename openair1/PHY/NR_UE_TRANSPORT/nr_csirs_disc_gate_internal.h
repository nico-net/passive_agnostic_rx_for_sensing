/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CSIRS_DISC_GATE_INTERNAL_H
#define NR_CSIRS_DISC_GATE_INTERNAL_H
#include "nr_csirs_disc.h"
#include <math.h>
#include <stdlib.h>

typedef struct {
  float k, m, floor, fixed;
} nr_csirs_disc_gate_t;

static inline float nr_csirs_disc_gate_env(const char *name, float fallback, bool zero_ok)
{
  const char *s = getenv(name);
  if (!s || !*s) return fallback;
  char *end;
  float v = strtof(s, &end);
  return end != s && !*end && isfinite(v) && (zero_ok ? v >= 0 : v > 0) ? v : fallback;
}

/* Read once per batch, shared by admission and refined re-verification. */
static inline nr_csirs_disc_gate_t nr_csirs_disc_gate_config(void)
{
  nr_csirs_disc_gate_t c;
  c.k = nr_csirs_disc_gate_env("ISAC_CSIRS_DISC_WIN_K", 12.0f, true);
  c.m = nr_csirs_disc_gate_env("ISAC_CSIRS_DISC_WIN_M", 4.0f, true);
  c.floor = nr_csirs_disc_gate_env("ISAC_CSIRS_DISC_WIN_FLOOR", 2.0f, false);
  c.fixed = nr_csirs_disc_gate_env("ISAC_CSIRS_DISC_WIN_Z", 0.0f, false);
  return c;
}

static inline bool nr_csirs_disc_gate_accept(const nr_csirs_disc_win_t *w, const nr_csirs_disc_gate_t *c)
{
  if (!isfinite(w->z) || !isfinite(w->z_second) || !w->n_slots)
    return false;
  /* Explicit WIN_Z preserves the legacy absolute 1.5 margin, too. */
  if (c->fixed > 0)
    return w->z >= c->fixed && w->z - w->z_second >= 1.5f;
  if (w->noise_count < 2 || !isfinite(w->noise_mu) || !isfinite(w->noise_sigma)
      || w->noise_mu < 0 || w->noise_sigma < 0)
    return false;
  return w->z >= c->floor && w->z >= w->noise_mu + c->k * w->noise_sigma
      && w->z - w->z_second >= c->m * w->noise_sigma;
}
#endif
