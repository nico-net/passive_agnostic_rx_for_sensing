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

/*! \file openair1/SIMULATION/TOOLS/sensing_channel.c
 * \brief Synthetic moving-target sensing channel — implementation. See sensing_channel.h.
 *
 * Physics / DSP rationale (stated here so it isn't re-litigated):
 *
 *  - The tap phase is derived from the ABSOLUTE, continuous bistatic path length R(t), NOT by
 *    integrating a velocity-derived instantaneous Doppler. Piecewise-linear position => R(t) is
 *    continuous even where velocity (its derivative) is not, so exp(-j 2*pi R(t)/lambda) is
 *    continuous through a trajectory corner: the phase never jumps, only its slope (the Doppler)
 *    changes -- the faithful signature of a maneuvering target, not an artifact. Computing R(t) in
 *    closed form each block is also drift-free (unlike phase accumulation over a long run).
 *
 *  - The Doppler is intentionally NOT written into channel_desc_t.Doppler_phase_inc (a single scalar
 *    that rxAddInput() applies to the whole convolved output -- it cannot give different objects
 *    different Doppler). Instead each object's Doppler emerges from the block-to-block progression of
 *    its own tap phase. rxAddInput() is left completely untouched; Doppler_phase_inc stays 0.
 *
 *  - Range migration uses a Hann-windowed sinc FRACTIONAL-delay kernel, not nearest-bin round().
 *    Rounding snaps a moving target's energy abruptly between bins; a snap mid-CPI is a slow-time
 *    step that smears across the Doppler axis. The windowed sinc keeps a target a clean band-limited
 *    impulse at fractional delay tau and lets it slide smoothly across bins. frac_delay_taps=0 falls
 *    back to nearest-bin (kept so the snapping artifact can be shown present->absent in tests).
 *
 *  - The trajectory is evaluated at each block's MIDPOINT time, so one CIR represents the whole
 *    convolved block with minimal bias.
 */

#include "sensing_channel.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config/config_userapi.h"
#include "common/platform_constants.h" /* SPEED_OF_LIGHT */
#include "common/utils/LOG/log.h"

// ---------------------------------------------------------------------------------------------
// Private trajectory representation
// ---------------------------------------------------------------------------------------------

typedef struct {
  double t; ///< waypoint time, seconds from channel start
  double x; ///< ENU x, metres (same frame as the ISAC [sensing] rx_pos_*/tx_pos_* fields)
  double y; ///< ENU y, metres
} sens_waypoint_t;

typedef struct {
  double           refl; ///< linear reflectivity (tap amplitude relative to LOS gain of 1.0)
  sens_waypoint_t *wp;   ///< waypoints, ascending in t
  int              nwp;
} sens_object_t;

typedef struct sensing_traj_s {
  double tx_x, tx_y; ///< illuminator (gNB) ENU position, m
  double rx_x, rx_y; ///< receiver (UE) ENU position, m
  double r_los;      ///< |TX - RX|, m (direct path length, the differential-delay origin)
  double lambda;     ///< carrier wavelength = c / center_freq, m
  double fs;         ///< sampling rate, samples/s
  double los_gain;   ///< linear LOS tap amplitude
  double los_delay;  ///< LOS delay in samples (differential-delay origin; usually 0)
  int    frac_taps;  ///< Hann-windowed sinc half-width; 0 => nearest-bin round()
  int    channel_length; ///< tap count owned by this channel (>= max object delay + frac_taps + 1)

  sens_object_t *obj;
  int            nobj;

  int      started;   ///< false until the first update() call anchors start_TS
  uint64_t start_TS;  ///< block timestamp of the first update() call
  double   last_log_t; ///< last ground-truth-log time (s), for once-per-second logging
} sensing_traj_t;

// ---------------------------------------------------------------------------------------------
// Small math helpers
// ---------------------------------------------------------------------------------------------

static inline double sinc_norm(double x)
{
  if (fabs(x) < 1e-12) {
    return 1.0;
  }
  const double px = M_PI * x;
  return sin(px) / px;
}

// Raised-cosine (Hann-family) taper over +/-(H+1), ~0 at the kernel support edges. Suppresses the
// truncation ripple of the finite sinc so a static target reads as a clean point.
static inline double frac_window(double d, int H)
{
  const double edge = (double)(H + 1);
  if (fabs(d) >= edge) {
    return 0.0;
  }
  return 0.5 * (1.0 + cos(M_PI * d / edge));
}

// Linear interpolation of an object's position at time t (piecewise-linear, half-open segments,
// endpoint hold outside the waypoint range). Position is continuous at waypoints, so the boundary
// tie-break is immaterial.
static void object_position(const sens_object_t *o, double t, double *x, double *y)
{
  if (o->nwp <= 0) {
    *x = 0.0;
    *y = 0.0;
    return;
  }
  if (o->nwp == 1 || t <= o->wp[0].t) {
    *x = o->wp[0].x;
    *y = o->wp[0].y;
    return;
  }
  if (t >= o->wp[o->nwp - 1].t) {
    *x = o->wp[o->nwp - 1].x;
    *y = o->wp[o->nwp - 1].y;
    return;
  }
  int i = 0;
  while (i < o->nwp - 1 && !(t >= o->wp[i].t && t < o->wp[i + 1].t)) {
    i++;
  }
  const double t0 = o->wp[i].t;
  const double t1 = o->wp[i + 1].t;
  const double a  = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0;
  *x = o->wp[i].x + a * (o->wp[i + 1].x - o->wp[i].x);
  *y = o->wp[i].y + a * (o->wp[i + 1].y - o->wp[i].y);
}

static double bistatic_range(const sensing_traj_t *s, double ox, double oy)
{
  const double d_tx = hypot(s->tx_x - ox, s->tx_y - oy);
  const double d_rx = hypot(ox - s->rx_x, oy - s->rx_y);
  return d_tx + d_rx;
}

// R(t) for one object, for the ground-truth finite-difference range-rate.
static double object_bistatic_range(const sensing_traj_t *s, const sens_object_t *o, double t)
{
  double ox, oy;
  object_position(o, t, &ox, &oy);
  return bistatic_range(s, ox, oy);
}

// ---------------------------------------------------------------------------------------------
// Tap synthesis
// ---------------------------------------------------------------------------------------------

static inline void accum_tap(channel_desc_t *cd, int k, double re, double im)
{
  if (k < 0 || k >= (int)cd->channel_length) {
    return;
  }
  const int npairs = cd->nb_tx * cd->nb_rx;
  for (int p = 0; p < npairs; p++) {
    cd->ch[p][k].r += re;
    cd->ch[p][k].i += im;
  }
}

// Add complex gain (gr,gi) at continuous fractional delay tau, spread with a Hann-windowed sinc.
static void add_tap_frac(channel_desc_t *cd, const sensing_traj_t *s, double tau, double gr, double gi)
{
  if (s->frac_taps <= 0) {
    accum_tap(cd, (int)lround(tau), gr, gi);
    return;
  }
  const int k0 = (int)floor(tau);
  const int H  = s->frac_taps;
  for (int k = k0 - H; k <= k0 + H; k++) {
    const double d = (double)k - tau;
    const double w = sinc_norm(d) * frac_window(d, H);
    if (w != 0.0) {
      accum_tap(cd, k, gr * w, gi * w);
    }
  }
}

void sensing_channel_update(channel_desc_t *cd, int nbSamples, uint64_t TS)
{
  sensing_traj_t *s = (sensing_traj_t *)cd->sensing_traj;
  if (s == NULL) {
    return;
  }
  if (!s->started) {
    s->start_TS   = TS;
    s->started    = 1;
    s->last_log_t = -1e9;
  }

  // Trajectory time at the block midpoint.
  const double t = ((double)(TS - s->start_TS) + (double)nbSamples / 2.0) / s->fs;
  const double c = (double)SPEED_OF_LIGHT;

  // Zero all taps for every tx/rx antenna pair.
  const int npairs = cd->nb_tx * cd->nb_rx;
  for (int p = 0; p < npairs; p++) {
    memset(cd->ch[p], 0, (size_t)cd->channel_length * sizeof(cd->ch[p][0]));
  }

  // Static direct path (LOS): real gain, zero Doppler, at the differential-delay origin.
  add_tap_frac(cd, s, s->los_delay, s->los_gain, 0.0);

  // Moving reflectors.
  for (int k = 0; k < s->nobj; k++) {
    double ox, oy;
    object_position(&s->obj[k], t, &ox, &oy);
    const double R   = bistatic_range(s, ox, oy);
    const double dR  = R - s->r_los;              // differential range vs. direct path
    const double tau = s->los_delay + dR / c * s->fs; // continuous fractional delay, samples
    const double phase = -2.0 * M_PI * R / s->lambda; // carrier phase for a path of length R
    const double gr    = s->obj[k].refl * cos(phase);
    const double gi    = s->obj[k].refl * sin(phase);
    add_tap_frac(cd, s, tau, gr, gi);
  }

  // Doppler is carried by the tap phases (block-to-block); leave the scalar rotation off.
  cd->Doppler_phase_inc = 0.0;

  // Ground-truth log once per simulated second: each object's true bistatic differential range and
  // range-rate (centered finite difference of R, so it's well-defined even sitting on a corner).
  if (t - s->last_log_t >= 1.0) {
    s->last_log_t = t;
    const double delta = 1e-3;
    for (int k = 0; k < s->nobj; k++) {
      const double R  = object_bistatic_range(s, &s->obj[k], t);
      const double Rp = object_bistatic_range(s, &s->obj[k], t + delta);
      const double Rm = object_bistatic_range(s, &s->obj[k], t - delta);
      const double range_rate = (Rp - Rm) / (2.0 * delta); // d(R)/dt = d(dR)/dt, m/s
      double ox, oy;
      object_position(&s->obj[k], t, &ox, &oy);
      LOG_I(HW,
            "SENSING_CHANNEL gt: t=%.2fs obj%d pos=(%.1f,%.1f)m bistatic_range=%.2fm dR=%.2fm "
            "range_rate=%.3fm/s (expect detection near range=%.2fm)\n",
            t, k, ox, oy, R, R - s->r_los, range_rate, R - s->r_los);
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Config parsing + lifecycle
// ---------------------------------------------------------------------------------------------

// Parse one object string "refl;t0,x0,y0;t1,x1,y1;...". Returns 0 on success.
static int parse_object(const char *spec, sens_object_t *out)
{
  memset(out, 0, sizeof(*out));
  char *dup = strdup(spec);
  if (dup == NULL) {
    return -1;
  }

  // First ';'-field is reflectivity; the rest are waypoints.
  char *saveptr = NULL;
  char *field   = strtok_r(dup, ";", &saveptr);
  if (field == NULL || sscanf(field, "%lf", &out->refl) != 1) {
    free(dup);
    return -1;
  }

  int cap = 4;
  out->wp = (sens_waypoint_t *)calloc(cap, sizeof(sens_waypoint_t));
  out->nwp = 0;
  while ((field = strtok_r(NULL, ";", &saveptr)) != NULL) {
    double tt, xx, yy;
    if (sscanf(field, "%lf,%lf,%lf", &tt, &xx, &yy) != 3) {
      continue; // skip malformed waypoint
    }
    if (out->nwp == cap) {
      cap *= 2;
      out->wp = (sens_waypoint_t *)realloc(out->wp, (size_t)cap * sizeof(sens_waypoint_t));
    }
    out->wp[out->nwp].t = tt;
    out->wp[out->nwp].x = xx;
    out->wp[out->nwp].y = yy;
    out->nwp++;
  }
  free(dup);

  if (out->nwp < 1) {
    free(out->wp);
    out->wp = NULL;
    return -1;
  }
  return 0;
}

// channel_desc_t.channel_length is a uint8_t (shared struct, used by all OAI channel models), so the
// CIR is capped at SENS_MAX_TAPS taps. At the gNB sample rate this still covers >1 km of bistatic
// differential range (255 taps / 61.44 MHz * c ~= 1244 m), ample for realistic sensing scenes.
#define SENS_MAX_TAPS 255

// Grow cd->ch[] (and channel_length) to at least `need` taps so object delays fit. The base rfsim
// model type (e.g. AWGN) may allocate too few taps for a sensing scene. Clamps to SENS_MAX_TAPS.
static void ensure_channel_length(channel_desc_t *cd, int need)
{
  if (need > SENS_MAX_TAPS) {
    LOG_W(HW,
          "SENSING_CHANNEL: requested %d taps exceeds the uint8_t channel_length cap (%d); clamping. Targets "
          "beyond ~%.0f m bistatic differential range will be dropped.\n",
          need, SENS_MAX_TAPS, (double)SENS_MAX_TAPS * (double)SPEED_OF_LIGHT / cd->sampling_rate);
    need = SENS_MAX_TAPS;
  }
  if (need <= (int)cd->channel_length) {
    return;
  }
  const int npairs = cd->nb_tx * cd->nb_rx;
  for (int p = 0; p < npairs; p++) {
    struct complexd *grown = (struct complexd *)realloc(cd->ch[p], (size_t)need * sizeof(struct complexd));
    // Zero the newly added tail.
    memset(&grown[cd->channel_length], 0, (size_t)(need - cd->channel_length) * sizeof(struct complexd));
    cd->ch[p] = grown;
  }
  LOG_I(HW, "SENSING_CHANNEL: grew channel_length %u -> %d taps for sensing scene\n", cd->channel_length, need);
  cd->channel_length = (uint8_t)need;
}

void *sensing_channel_make(channel_desc_t *cd,
                           double          tx_x,
                           double          tx_y,
                           double          rx_x,
                           double          rx_y,
                           double          los_gain_db,
                           double          los_delay_samples,
                           int             frac_delay_taps,
                           int             channel_length,
                           const char     *objects_spec)
{
  if (cd->center_freq == 0 || cd->sampling_rate <= 0.0) {
    LOG_E(HW, "SENSING_CHANNEL: cannot enable (center_freq=%lu sampling_rate=%f invalid)\n",
          (unsigned long)cd->center_freq, cd->sampling_rate);
    return NULL;
  }

  sensing_traj_t *s = (sensing_traj_t *)calloc(1, sizeof(sensing_traj_t));
  s->tx_x      = tx_x;
  s->tx_y      = tx_y;
  s->rx_x      = rx_x;
  s->rx_y      = rx_y;
  s->r_los     = hypot(s->tx_x - s->rx_x, s->tx_y - s->rx_y);
  s->lambda    = (double)SPEED_OF_LIGHT / (double)cd->center_freq;
  s->fs        = cd->sampling_rate;
  s->los_gain  = pow(10.0, los_gain_db / 20.0);
  s->los_delay = los_delay_samples;
  s->frac_taps = (frac_delay_taps > 0) ? frac_delay_taps : 0;
  s->started   = 0;

  // Parse the '|'-separated object list.
  if (objects_spec != NULL && objects_spec[0] != '\0') {
    char *dup     = strdup(objects_spec);
    int   cap     = 4;
    s->obj        = (sens_object_t *)calloc(cap, sizeof(sens_object_t));
    s->nobj       = 0;
    char *saveptr = NULL;
    for (char *tok = strtok_r(dup, "|", &saveptr); tok != NULL; tok = strtok_r(NULL, "|", &saveptr)) {
      // Trim leading spaces.
      while (*tok == ' ') {
        tok++;
      }
      if (*tok == '\0') {
        continue;
      }
      sens_object_t o;
      if (parse_object(tok, &o) == 0) {
        if (s->nobj == cap) {
          cap *= 2;
          s->obj = (sens_object_t *)realloc(s->obj, (size_t)cap * sizeof(sens_object_t));
        }
        s->obj[s->nobj++] = o;
      } else {
        LOG_W(HW, "SENSING_CHANNEL: ignoring malformed object spec '%s'\n", tok);
      }
    }
    free(dup);
  }

  // Size the CIR: cover the largest object differential delay seen across all waypoints, plus the
  // fractional-kernel half-width and a couple of guard taps. Then honour the configured minimum.
  double max_tau = s->los_delay;
  for (int k = 0; k < s->nobj; k++) {
    for (int w = 0; w < s->obj[k].nwp; w++) {
      const double R   = bistatic_range(s, s->obj[k].wp[w].x, s->obj[k].wp[w].y);
      const double tau = s->los_delay + (R - s->r_los) / (double)SPEED_OF_LIGHT * s->fs;
      if (tau > max_tau) {
        max_tau = tau;
      }
    }
  }
  int need = (int)ceil(max_tau) + s->frac_taps + 2;
  if (need < channel_length) {
    need = channel_length;
  }
  ensure_channel_length(cd, need);
  s->channel_length = cd->channel_length;

  LOG_I(HW,
        "SENSING_CHANNEL: enabled TX=(%.1f,%.1f) RX=(%.1f,%.1f) R_los=%.2fm lambda=%.4fm fs=%.3fMsps "
        "los_gain=%.3f frac_taps=%d objects=%d channel_length=%d\n",
        s->tx_x, s->tx_y, s->rx_x, s->rx_y, s->r_los, s->lambda, s->fs / 1e6, s->los_gain, s->frac_taps, s->nobj,
        cd->channel_length);
  for (int k = 0; k < s->nobj; k++) {
    LOG_I(HW, "SENSING_CHANNEL:   obj%d refl=%.3f waypoints=%d (t0=%.2fs pos0=(%.1f,%.1f))\n", k, s->obj[k].refl,
          s->obj[k].nwp, s->obj[k].wp[0].t, s->obj[k].wp[0].x, s->obj[k].wp[0].y);
  }
  return s;
}

void *sensing_channel_parse(channel_desc_t *cd)
{
  int    p_enable   = 0;
  int    p_frac     = 8;
  int    p_chanlen  = 128;
  double p_tx_x = 0.0, p_tx_y = 0.0, p_rx_x = 0.0, p_rx_y = 0.0;
  double p_los_db = 0.0, p_los_delay = 0.0;
  char  *p_objects = NULL;

  paramdef_t params[] = {
      {"enable", "enable synthetic moving-target sensing channel", PARAMFLAG_BOOL, .iptr = &p_enable, .defintval = 0,
       TYPE_INT, 0},
      {"tx_pos_x", "illuminator ENU x (m)", 0, .dblptr = &p_tx_x, .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"tx_pos_y", "illuminator ENU y (m)", 0, .dblptr = &p_tx_y, .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"rx_pos_x", "receiver ENU x (m)", 0, .dblptr = &p_rx_x, .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"rx_pos_y", "receiver ENU y (m)", 0, .dblptr = &p_rx_y, .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"los_gain_db", "LOS/direct-path gain (dB, 0 = unit)", 0, .dblptr = &p_los_db, .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"los_delay_samples", "LOS delay = differential-range origin (samples)", 0, .dblptr = &p_los_delay,
       .defdblval = 0.0, TYPE_DOUBLE, 0},
      {"frac_delay_taps", "windowed-sinc fractional-delay half-width (0 = nearest bin)", 0, .iptr = &p_frac,
       .defintval = 8, TYPE_INT, 0},
      {"channel_length", "tap count for the sensing CIR (<= 255, uint8_t cap)", 0, .iptr = &p_chanlen,
       .defintval = 128, TYPE_INT, 0},
      {"objects", "moving targets: \"refl;t,x,y;t,x,y;...\" per object, '|'-separated", 0, .strptr = &p_objects,
       .defstrval = "", TYPE_STRING, 0},
  };
  const int nparams = (int)(sizeof(params) / sizeof(params[0]));
  config_get(config_get_if(), params, nparams, SENSING_CHANNEL_SECTION);

  if (!p_enable) {
    return NULL;
  }
  return sensing_channel_make(cd, p_tx_x, p_tx_y, p_rx_x, p_rx_y, p_los_db, p_los_delay, p_frac, p_chanlen, p_objects);
}

void sensing_channel_free(void *traj)
{
  sensing_traj_t *s = (sensing_traj_t *)traj;
  if (s == NULL) {
    return;
  }
  for (int k = 0; k < s->nobj; k++) {
    free(s->obj[k].wp);
  }
  free(s->obj);
  free(s);
}
