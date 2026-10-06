/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_sweep_internal.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int valid_hyp(const nr_csirs_disc_hyp_t *h, int n_rb)
{
  static const uint8_t bits[] = {0,1,1,1,1,1,4,2,2,6,3,4,4,3,3,3,4,4,4};
  static const uint8_t cdm[] = {0,0,0,1,1,1,1,1,2,1,2,1,2,1,2,3,1,2,3};
  if (h->row < 1 || h->row > 18 || h->cdm_type != cdm[h->row] || h->density > 3
      || !h->n_rb || h->rb_start + h->n_rb > n_rb || !h->period || h->offset >= h->period)
    return 0;
  int width = h->row == 1 ? 4 : h->row == 2 ? 12 : h->row == 4 ? 3 : 6;
  if ((h->freq_domain >> width) || __builtin_popcount(h->freq_domain) != bits[h->row])
    return 0;
  if ((h->row == 1 && h->density != 3) || (h->row != 1 && h->density == 3))
    return 0;
  const csi_mapping_parms_t m = get_csi_mapping_parms(h->row, h->freq_domain, h->symb_l0, h->symb_l1);
  for (int j = 0; j < m.size; ++j)
    if (m.loverline[j] + m.lprime >= NR_CSIRS_DISC_NSYM || m.koverline[j] + m.kprime >= 12)
      return 0;
  return 1;
}

int nr_csirs_disc_sweep_validate(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *h, int nh,
                                const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_sweep_cfg_t *c)
{
  if (!r || !h || !c || nh < 1 || nh > NR_CSIRS_DISC_MAX_HYP || !r->grid_base || !r->meta
      || r->n_ant < 1 || r->n_ant > NR_CSIRS_DISC_MAX_ANT || r->n_rb < 1 || r->n_rb > NR_CSIRS_DISC_MAX_RB
      || r->capacity < 1 || r->capacity > UINT16_MAX || r->slots_per_frame < 1 || r->slots_per_frame > 160
      || r->slot_stride < (size_t)r->n_ant * 14 * 24 * r->n_rb
      || r->slot_stride > SIZE_MAX / sizeof(int16_t) / r->capacity)
    return -1;
  *c = cfg ? *cfg : (nr_csirs_disc_sweep_cfg_t){0,1024,1,32};
  if (c->id_first < 0 || c->id_count < 1 || c->id_first > 1023 || c->id_count > 1024 - c->id_first
      || c->delta_max < 1 || c->delta_max > r->slots_per_frame || c->block_re < 1)
    return -1;
  for (int i = 0; i < r->capacity; ++i)
    if (r->meta[i].valid && r->meta[i].slot >= r->slots_per_frame)
      return -1;
  for (int i = 0; i < nh; ++i)
    if (!valid_hyp(h + i, r->n_rb))
      return -1;
  return 0;
}

static int re_order(const void *a, const void *b)
{
  return ((const nr_csirs_disc_re_t *)a)->grid_index - ((const nr_csirs_disc_re_t *)b)->grid_index;
}

int nr_csirs_disc_sweep_map(const nr_csirs_disc_hyp_t *h, int n_rb, nr_csirs_disc_sweep_map_t *out)
{
  memset(out, 0, sizeof(*out));
  if (!valid_hyp(h, n_rb))
    return -1;
  const csi_mapping_parms_t m = get_csi_mapping_parms(h->row, h->freq_domain, h->symb_l0, h->symb_l1);
  const int gs = get_cdm_group_size(h->cdm_type);
  const double rho = h->density < 2 ? 0.5 : h->density == 2 ? 1 : 3;
  const double alpha = m.ports == 1 ? rho : 2 * rho;
  const size_t cap = (size_t)h->n_rb * m.size * gs * (m.kprime + 1) * (m.lprime + 1);
  out->re = malloc(cap * sizeof(*out->re));
  if (!out->re)
    return -1;
  out->ports = m.ports;
  for (int p = 0; p < m.ports; ++p) {
    out->begin[p] = out->n_re;
    int s = p % gs;
    for (int rb = h->rb_start; rb < h->rb_start + h->n_rb; ++rb) {
      if (h->density < 2 && rb % 2 != h->density)
        continue;
      for (int j = 0; j < m.size; ++j) {
        if (m.j[j] != p / gs)
          continue;
        for (int kp = 0; kp <= m.kprime; ++kp)
          for (int lp = 0; lp <= m.lprime; ++lp) {
            int wf = kp == 0 ? 1 : 1 - 2 * (s % 2);
            int wt = s < 2 ? 1 : s < 4 ? 1 - 2 * (lp % 2) : s < 6 ? 1 - 2 * (lp / 2)
                                                                                        : (lp == 0 || lp == 3 ? 1 : -1);
            int l = m.loverline[j] + lp;
            int seq = (int)(rb * alpha) + kp + (int)(rho * m.koverline[j] / 12);
            out->re[out->n_re++] = (nr_csirs_disc_re_t){l * 12 * n_rb + rb * 12 + m.koverline[j] + kp,
                                                     seq, l, wf * wt};
            out->sym_mask |= 1u << l;
            if (seq / 16 + 1 > out->words)
              out->words = seq / 16 + 1;
          }
      }
    }
    qsort(out->re + out->begin[p], out->n_re - out->begin[p], sizeof(*out->re), re_order);
  }
  out->begin[m.ports] = out->n_re;
  return 0;
}

void nr_csirs_disc_sweep_winner(const float *z, const nr_csirs_disc_sweep_cfg_t *c, int h, int slots,
                              nr_csirs_disc_win_t *w)
{
  *w = (nr_csirs_disc_win_t){.hyp_index = h, .scramb_id = c->id_first, .n_slots = slots};
  for (int id = 0; id < c->id_count; ++id) {
    float best = 0;
    int delta = 0;
    for (int d = 0; d < c->delta_max; ++d)
      if (z[id * c->delta_max + d] > best) {
        best = z[id * c->delta_max + d];
        delta = d;
      }
    if (best > w->z) {
      w->z_second = w->z;
      w->z = best;
      w->scramb_id = c->id_first + id;
      w->slot_delta = delta;
    } else if (best > w->z_second) {
      w->z_second = best;
    }
  }
}

/* Same magnitude/energy statistic, block flushing and Rayleigh normalization as
 * nr_csirs_blind_correlate_blocks_shift, on the compact occupied RE list. */
static double port_score(const int16_t *rx, const c16_t *ref, const nr_csirs_disc_re_t *re, int n, int block)
{
  double sum = 0;
  int blocks = 0;
  for (int first = 0; first < n;) {
    int end = n - first < block ? n : first + block;
    double ar = 0, ai = 0, er = 0, ex = 0;
    for (int i = first; i < end; ++i) {
      int k = re[i].grid_index;
      double yr = rx[2*k], yi = rx[2*k+1], xr = ref[k].r, xi = ref[k].i;
      ar += yr*xr + yi*xi;
      ai += yi*xr - yr*xi;
      er += yr*yr + yi*yi;
      ex += xr*xr + xi*xi;
    }
    if (er > 0 && ex > 0) {
      sum += fmin(1, sqrt((ar*ar + ai*ai) / (er*ex)));
      ++blocks;
    }
    first = end;
  }
  return blocks ? sum / blocks * sqrt((double)n / blocks) / 0.886 : 0;
}

int nr_csirs_disc_sweep_cpu(const nr_csirs_disc_ring_t *r, const nr_csirs_disc_hyp_t *hyps, int nh,
                          const nr_csirs_disc_sweep_cfg_t *cfg, nr_csirs_disc_win_t *out)
{
  const char *enable = getenv("ISAC_CSIRS_DISC");
  if (!enable || strcmp(enable, "1"))
    return 0;
  nr_csirs_disc_sweep_cfg_t c;
  if (!out || nr_csirs_disc_sweep_validate(r, hyps, nh, cfg, &c))
    return -1;
  NR_DL_FRAME_PARMS fp = {.N_RB_DL = r->n_rb, .ofdm_symbol_size = 12*r->n_rb, .symbols_per_slot = 14,
                          .slots_per_frame = r->slots_per_frame};
  int plane = 14 * 12 * r->n_rb;
  float *scores = calloc((size_t)c.id_count * c.delta_max, sizeof(*scores));
  if (!scores)
    return -1;
  for (int h = 0; h < nh; ++h) {
    const nr_csirs_disc_hyp_t *hyp = hyps + h;
    nr_csirs_disc_sweep_map_t map;
    if (nr_csirs_disc_sweep_map(hyp, r->n_rb, &map)) {
      free(scores);
      return -1;
    }
    c16_t *buf = calloc((size_t)plane * map.ports, sizeof(*buf));
    if (!buf) {
      free(map.re);
      free(scores);
      return -1;
    }
    c16_t *ports[32];
    for (int p = 0; p < map.ports; ++p)
      ports[p] = buf + (size_t)p * plane;
    const csi_mapping_parms_t mapping = get_csi_mapping_parms(hyp->row, hyp->freq_domain, hyp->symb_l0, hyp->symb_l1);
    /* OAI's even-half-density modulation length floors end_rb/2. For an odd
     * exclusive end it otherwise leaves the final even RB's sequence uninitialized.
     * Extend through the following odd (unoccupied) RB: no extra RE is mapped. */
    int generate_rbs = hyp->n_rb + (hyp->density == 0 && ((hyp->rb_start + hyp->n_rb) & 1));
    int slots = 0;
    for (int s = 0; s < r->capacity; ++s)
      slots += r->meta[s].valid && r->meta[s].abs_slot % hyp->period == hyp->offset;
    for (int id = 0; id < c.id_count; ++id)
      for (int d = 0; d < c.delta_max; ++d) {
        double total = 0;
        for (int s = 0; s < r->capacity; ++s) {
          if (!r->meta[s].valid || r->meta[s].abs_slot % hyp->period != hyp->offset)
            continue;
          nr_generate_csi_rs(&fp, &mapping, 16384, (r->meta[s].slot + d) % r->slots_per_frame,
                             hyp->density, hyp->rb_start, generate_rbs, hyp->symb_l0, hyp->symb_l1, hyp->row,
                             c.id_first + id, 1, hyp->cdm_type, ports);
          for (int p = 0; p < map.ports; ++p)
            for (int a = 0; a < r->n_ant; ++a)
              total += port_score(nr_csirs_disc_ring_sym(r, s, a, 0), ports[p], map.re + map.begin[p],
                                  map.begin[p+1] - map.begin[p], c.block_re);
        }
        scores[id*c.delta_max+d] = slots ? total / (slots * map.ports * r->n_ant) : 0;
      }
    nr_csirs_disc_sweep_winner(scores, &c, h, slots, out + h);
    free(buf);
    free(map.re);
  }
  free(scores);
  return nh;
}
