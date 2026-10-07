/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_sweep_internal.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include <math.h>
#include "PHY/gold.h"
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
      || c->max_slots < 0 || c->delta_max < 1 || c->delta_max > r->slots_per_frame || c->block_re < 1)
    return -1;
  for (int i = 0; i < r->capacity; ++i)
    if (r->meta[i].valid && r->meta[i].slot >= r->slots_per_frame)
      return -1;
  for (int i = 0; i < nh; ++i)
    if (!valid_hyp(h + i, r->n_rb))
      return -1;
  return 0;
}

static int slot_order(const void *a,const void *b)
{
  uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
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
  if (!slots || c->id_count < 2)
    return;
  /* Shift by a retained score before accumulating moments: this also stays
   * accurate when the distribution is nearly constant. */
  const double origin = z[w->scramb_id == c->id_first ? c->delta_max : 0];
  double sum = 0, sq = 0;
  for (int id = 0; id < c->id_count; ++id)
    if (c->id_first + id != w->scramb_id)
      for (int d = 0; d < c->delta_max; ++d) {
        const double v = (double)z[id * c->delta_max + d] - origin;
        sum += v;
        sq += v * v;
      }
  w->noise_count = (c->id_count - 1) * c->delta_max;
  const double mu = sum / w->noise_count;
  w->noise_mu = origin + mu;
  w->noise_sigma = sqrt(fmax(0, sq / w->noise_count - mu * mu));
}

/* Same magnitude/energy statistic, block flushing and Rayleigh normalization as
 * nr_csirs_blind_correlate_blocks_shift, on the compact occupied RE list. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("O3")))
#endif
static double port_score(const int16_t *rx, const int8_t *ref, int n, int block, const double *inverse)
{
  double sum = 0;
  int blocks = 0;
  for (int first = 0; first < n;) {
    int end = n - first < block ? n : first + block;
    /* Max legal per-port mapping is 2200 RE: 32-bit dot sums cannot overflow. */
    int32_t ar = 0, ai = 0;
    for (int i = first; i < end; ++i) {
      int yr=rx[2*i], yi=rx[2*i+1], xr=ref[2*i], xi=ref[2*i+1];
      ar += yr*xr + yi*xi;
      ai += yi*xr - yr*xi;
    }
    if (inverse[first] > 0) {
      sum += fmin(1, sqrt(((double)ar*ar + (double)ai*ai) * inverse[first]));
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
  if (!enable || strcmp(enable, "1")) return 0;
  nr_csirs_disc_sweep_cfg_t c;
  if (!out || nr_csirs_disc_sweep_validate(r, hyps, nh, cfg, &c)) return -1;
  float *scores = calloc((size_t)c.id_count*c.delta_max,sizeof(*scores));
  if (!scores) return -1;
  for (int h=0;h<nh;++h) {
    const nr_csirs_disc_hyp_t *hyp=hyps+h;
    nr_csirs_disc_sweep_map_t map;
    if (nr_csirs_disc_sweep_map(hyp,r->n_rb,&map)) {free(scores);return -1;}
    int slots=0;
    uint64_t *order=malloc((size_t)r->capacity*sizeof(*order));
    if(!order) {free(map.re);free(scores);return -1;}
    for(int s=0;s<r->capacity;++s)
      if(r->meta[s].valid && r->meta[s].abs_slot%hyp->period==hyp->offset)
        order[slots++]=((uint64_t)r->meta[s].abs_slot<<32)|(uint32_t)s;
    int first=0;
    if(c.max_slots>0 && slots>c.max_slots) {
      qsort(order,slots,sizeof(*order),slot_order);first=slots-c.max_slots;slots=c.max_slots;
    }
    int *slot=malloc((size_t)(slots+1)*sizeof(*slot));
    int16_t *rx=malloc((size_t)(slots+1)*r->n_ant*map.n_re*2*sizeof(*rx));
    double *inverse=calloc((size_t)(slots+1)*r->n_ant*map.n_re,sizeof(*inverse));
    int8_t *ref=malloc((size_t)r->slots_per_frame*map.n_re*2);
    uint32_t *gold=malloc((size_t)14*map.words*sizeof(*gold));
    if(!slot || !rx || !inverse || !ref || !gold) {
      free(order);free(slot);free(rx);free(inverse);free(ref);free(gold);free(map.re);free(scores);return -1;
    }
    int at=0;
    for(int k=0;k<slots;++k) {
      int s=(uint32_t)order[first+k];
      slot[at]=r->meta[s].slot;
      for(int a=0;a<r->n_ant;++a) for(int i=0;i<map.n_re;++i) {
        const int16_t *v=nr_csirs_disc_ring_sym(r,s,a,0)+2*map.re[i].grid_index;
        size_t j=((size_t)at*r->n_ant+a)*map.n_re+i;
        rx[2*j]=v[0];rx[2*j+1]=v[1];
      }
      ++at;
    }
    /* RX energies do not depend on scramblingID; compute once, not 1024 times. */
    for(int s=0;s<slots;++s) for(int a=0;a<r->n_ant;++a) for(int p=0;p<map.ports;++p) {
      size_t base=((size_t)s*r->n_ant+a)*map.n_re;
      for(int first=map.begin[p];first<map.begin[p+1];) {
        int end=first+c.block_re<map.begin[p+1]?first+c.block_re:map.begin[p+1];
        double energy=0;
        for(int i=first;i<end;++i) {double yr=rx[2*(base+i)],yi=rx[2*(base+i)+1];energy+=yr*yr+yi*yi;}
        inverse[base+first]=energy>0?1.0/(2*energy*(end-first)):0;first=end;
      }
    }
    for(int id=0;id<c.id_count;++id) for(int d=0;d<c.delta_max;++d) {
      bool generated[160]={0}; double total=0;
      for(int s=0;s<slots;++s) {
        int ns=(slot[s]+d)%r->slots_per_frame;
        int8_t *reference=ref+(size_t)ns*map.n_re*2;
        if(!generated[ns]) {
          for(int l=0;l<14;++l) if(map.sym_mask&(1u<<l)) {
            uint64_t t=14*ns+l+1, identity=c.id_first+id;
            uint32_t x1=0,x2=((t*(2*identity+1)<<10)+identity)&0x7fffffff;
            for(int w=0;w<map.words;++w) gold[l*map.words+w]=gold_generic(&x1,&x2,w==0);
          }
          for(int i=0;i<map.n_re;++i) {
            const nr_csirs_disc_re_t *re=map.re+i;
            unsigned bits=(gold[re->symbol*map.words+re->m/16]>>(2*(re->m%16)))&3;
            reference[2*i]=(bits&1?-1:1)*re->sign;
            reference[2*i+1]=(bits&2?-1:1)*re->sign;
          }
          generated[ns]=true;
        }
        for(int p=0;p<map.ports;++p) for(int a=0;a<r->n_ant;++a)
          total+=port_score(rx+(((size_t)s*r->n_ant+a)*map.n_re+map.begin[p])*2,
                            reference+map.begin[p]*2,map.begin[p+1]-map.begin[p],c.block_re,inverse+((size_t)s*r->n_ant+a)*map.n_re+map.begin[p]);
      }
      scores[id*c.delta_max+d]=slots?total/(slots*map.ports*r->n_ant):0;
    }
    nr_csirs_disc_sweep_winner(scores,&c,h,slots,out+h);
    free(order);free(slot);free(rx);free(inverse);free(ref);free(gold);free(map.re);
  }
  free(scores);return nh;
}

static int refine_extent(const nr_csirs_disc_ring_t *r, nr_csirs_disc_hyp_t *h,
                         const nr_csirs_disc_win_t *win)
{
  nr_csirs_disc_hyp_t full=*h;full.rb_start=0;full.n_rb=r->n_rb;
  nr_csirs_disc_sweep_map_t map;
  if(nr_csirs_disc_sweep_map(&full,r->n_rb,&map)) return -1;
  double evidence[NR_CSIRS_DISC_MAX_RB]={0};
  uint32_t gold[14][64];
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    for(int l=0;l<14;++l) if(map.sym_mask&(1u<<l)) {
      uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+l+1;
      uint32_t x1=0,x2=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
      for(int w=0;w<map.words;++w) gold[l][w]=gold_generic(&x1,&x2,w==0);
    }
    for(int a=0;a<r->n_ant;++a) for(int p=0;p<map.ports;++p) {
      double cr[NR_CSIRS_DISC_MAX_RB]={0},ci[NR_CSIRS_DISC_MAX_RB]={0};
      int count[NR_CSIRS_DISC_MAX_RB]={0};
      const int16_t *rx=nr_csirs_disc_ring_sym(r,s,a,0);
      for(int i=map.begin[p];i<map.begin[p+1];++i) {
        const nr_csirs_disc_re_t *re=map.re+i;
        int rb=(re->grid_index%(12*r->n_rb))/12;
        unsigned bits=(gold[re->symbol][re->m/16]>>(2*(re->m%16)))&3;
        double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
        int xr=(bits&1?-1:1)*re->sign,xi=(bits&2?-1:1)*re->sign;
        cr[rb]+=yr*xr+yi*xi;ci[rb]+=yi*xr-yr*xi;++count[rb];
      }
      const int step=h->density<2?2:1;
      for(int rb=0;rb<r->n_rb;++rb) if(count[rb]) {
        double hr=0,hi=0;int n=0;
        for(int k=rb-2*step;k<=rb+2*step;k+=step) if(k>=0 && k<r->n_rb && k!=rb) {
          hr+=cr[k];hi+=ci[k];n+=count[k];
        }
        if(n) {
          hr/=2*n;hi/=2*n;
          // Leave this RB out of the channel estimate: noise has negative expected gain.
          evidence[rb]+=2*(cr[rb]*hr+ci[rb]*hi)-2*count[rb]*(hr*hr+hi*hi);
        }
      }
    }
  }
  int step=h->density<2?2:1,first=h->density<2?h->density:0;
  double run=0,best=0;int start=first,bs=0,be=-1;
  for(int rb=first;rb<r->n_rb;rb+=step) {
    if(run<=0) {run=evidence[rb];start=rb;} else run+=evidence[rb];
    if(run>best && rb>=h->rb_start && start<h->rb_start+h->n_rb) {best=run;bs=start;be=rb;}
  }
  free(map.re);
  if(be<bs) return -1;
  h->rb_start=bs;h->n_rb=be-bs+1;return 0;
}

/* Sequence-qualified row-1 extent. Energy finds a seed even when beamforming
 * gives pilots and traffic different notches. Once the ID is known, subtract
 * the QPSK null cross terms per RB and locate the connected support around that
 * seed. No configured bandwidth, PCI or symbol is an input to this refinement. */
int nr_csirs_disc_refine_cpu(const nr_csirs_disc_ring_t *r, nr_csirs_disc_hyp_t *h,
                           const nr_csirs_disc_win_t *win)
{
  if (h->row != 1) return refine_extent(r,h,win);
  nr_csirs_disc_hyp_t full=*h;full.rb_start=0;full.n_rb=r->n_rb;
  nr_csirs_disc_sweep_map_t map;
  if(nr_csirs_disc_sweep_map(&full,r->n_rb,&map)) return -1;
  double signal[NR_CSIRS_DISC_MAX_RB]={0}, weight[NR_CSIRS_DISC_MAX_RB]={0};
  uint32_t gold[64];
  int slots=0;
  for(int s=0;s<r->capacity;++s) if(r->meta[s].valid && r->meta[s].abs_slot%h->period==h->offset) {
    ++slots;
    uint64_t t=14*((r->meta[s].slot+win->slot_delta)%r->slots_per_frame)+h->symb_l0+1;
    uint32_t x1=0,x2=((t*(2*win->scramb_id+1)<<10)+win->scramb_id)&0x7fffffff;
    for(int w=0;w<map.words;++w) gold[w]=gold_generic(&x1,&x2,w==0);
    for(int a=0;a<r->n_ant;++a) for(int rb=0;rb<r->n_rb;++rb) {
      const int16_t *rx=nr_csirs_disc_ring_sym(r,s,a,0);
      double ar=0,ai=0,energy=0;
      for(int i=3*rb;i<3*rb+3;++i) {
        const nr_csirs_disc_re_t *re=map.re+i;
        unsigned bits=(gold[re->m/16]>>(2*(re->m%16)))&3;
        double yr=rx[2*re->grid_index],yi=rx[2*re->grid_index+1];
        int xr=bits&1?-1:1,xi=bits&2?-1:1;
        ar+=yr*xr+yi*xi;ai+=yi*xr-yr*xi;energy+=yr*yr+yi*yi;
      }
      /* Energy weighting naturally downweights a disconnected RX branch. */
      signal[rb]+=(ar*ar+ai*ai-2*energy)/4;
      weight[rb]+=energy;
    }
  }
  /* TS 38.214 5.2.2.3.1: configured startingRB/nrofRBs are multiples
   * of four. Maximize measured support over that legal grid; no width prior.
   * A capture boundary may clip the final group. */
  double run=0,best=0;int start=0,bs=0,be=-1;
  for(int rb=0;rb<r->n_rb;rb+=4) {
    int end=rb+4<r->n_rb?rb+4:r->n_rb;double e=0;
    for(int k=rb;k<end;++k) e+=weight[k]>0?signal[k]/weight[k]-0.35:-1;
    if(run<=0) {run=e;start=rb;} else run+=e;
    if(run>best && end>h->rb_start && start<h->rb_start+h->n_rb) {best=run;bs=start;be=end-1;}
  }
  free(map.re);
  if(slots<3 || be-bs+1<4) return -1;
  h->rb_start=bs;h->n_rb=be-bs+1;
  return 0;
}
