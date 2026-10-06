/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#include "nr_csirs_disc_synth.h"
#include "PHY/nr_phy_common/inc/nr_phy_common.h"
#include "common/utils/LOG/log.h"
#include "common/config/config_load_configmodule.h"
#include "common/config/config_userapi.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static void init_tables(void)
{
  if (!config_get_if()) {
    char name[]="csirs_disc_synth";
    char *argv[]={name,NULL};
    uniqCfg=load_configmodule(1,argv,CONFIG_ENABLECMDLINEONLY);
  }
  logInit();set_glog(OAILOG_ERR);nr_generate_modulation_table();
}
void nr_csirs_disc_synth_init(void) { static pthread_once_t once=PTHREAD_ONCE_INIT; pthread_once(&once,init_tables); }
static uint32_t rng(uint32_t *s) { *s ^= *s<<13; *s ^= *s>>17; *s ^= *s<<5; return *s; }
static double uniform(uint32_t *s) { return (rng(s)+1.0)/4294967297.0; }
static double gaussian(uint32_t *s) { return sqrt(-2*log(uniform(s)))*cos(6.283185307179586*uniform(s)); }
static int16_t quantize(double x) { return lrint(fmax(-32768,fmin(32767,x))); }
static int valid_resource(const nr_csirs_disc_synth_resource_t *c)
{
  static const int bits[18]={1,1,1,1,1,4,2,2,6,3,4,4,3,3,3,4,4,4};
  static const int cdm[18]={0,0,1,1,1,1,1,2,1,2,1,2,1,2,3,1,2,3};
  if(c->row<1 || c->row>18 || c->density>3 || c->cdm!=cdm[c->row-1] || c->l0>13 || c->l1>13 ||
     !c->n_rb || c->rb_start+c->n_rb>275 || !c->period || c->offset>=c->period ||
     !isfinite(c->power) || c->power<=0 || c->scramb_id>1023) return 0;
  if ((c->row==1) != (c->density==3) || (c->row>=4 && c->row<=10 && c->density!=2)) return 0;
  int width=c->row==1?4:c->row==2?12:c->row==4?3:6;
  if(c->fd_bitmap>>width || __builtin_popcount(c->fd_bitmap)!=bits[c->row-1]) return 0;
  csi_mapping_parms_t p=get_csi_mapping_parms(c->row,c->fd_bitmap,c->l0,c->l1);
  for(int j=0;j<p.size;++j) if(p.loverline[j]+p.lprime>13 || p.koverline[j]+p.kprime>11) return 0;
  return 1;
}
int nr_csirs_disc_synth_footprint(const nr_csirs_disc_synth_resource_t *c,nr_csirs_disc_fp_t *f)
{
  if(!c || !f || !valid_resource(c)) return -1;
  memset(f,0,sizeof(*f));
  csi_mapping_parms_t p=get_csi_mapping_parms(c->row,c->fd_bitmap,c->l0,c->l1);
  for(int j=0;j<p.size;++j) {
    for(int k=0;k<=p.kprime;++k) f->sc_mask|=1<<(p.koverline[j]+k);
    for(int l=0;l<=p.lprime;++l) f->sym_mask|=1<<(p.loverline[j]+l);
  }
  f->rb_start=c->rb_start;f->n_rb=c->n_rb;f->density=c->density;f->period=c->period;f->offset=c->offset;
  /* Canonical observable extent for half-density: silent boundary RBs are unknowable. */
  if(c->density<2) {
    int last=c->rb_start+c->n_rb-1;
    if(f->rb_start%2!=c->density) ++f->rb_start;
    if(last%2!=c->density) --last;
    f->n_rb=last>=f->rb_start?last-f->rb_start+1:0;
  }
  return 0;
}
void nr_csirs_disc_synth_free(nr_csirs_disc_ring_t *r)
{
  if(r) {free(r->grid_base);free(r->meta);memset(r,0,sizeof(*r));}
}
int nr_csirs_disc_synth(nr_csirs_disc_ring_t *r,const nr_csirs_disc_synth_cfg_t *cfg)
{
  if(!r || !cfg || cfg->n_slots<=0 || cfg->n_rb<1 || cfg->n_rb>275 || cfg->n_ant<1 || cfg->n_ant>4 ||
     cfg->slots_per_frame<1 || cfg->slots_per_frame>160 || cfg->n_resources<0 ||
     (cfg->n_resources && !cfg->resources) || !isfinite(cfg->snr_db) || !isfinite(cfg->load) || cfg->load<0 || cfg->load>1)
    return -1;
  for(int i=0;i<cfg->n_resources;++i) if(!valid_resource(cfg->resources+i) ||
       cfg->resources[i].rb_start+cfg->resources[i].n_rb>cfg->n_rb) return -1;
  memset(r,0,sizeof(*r));
  r->n_ant=cfg->n_ant;r->n_rb=cfg->n_rb;r->capacity=cfg->n_slots;r->slots_per_frame=cfg->slots_per_frame;
  r->slot_stride=(size_t)cfg->n_ant*14*24*cfg->n_rb;
  if((size_t)r->capacity>SIZE_MAX/r->slot_stride/sizeof(int16_t)) return -1;
  r->grid_base=calloc(r->capacity,r->slot_stride*sizeof(int16_t));r->meta=calloc(r->capacity,sizeof(*r->meta));
  int nk=12*r->n_rb,nre=14*nk;
  c16_t *ports=calloc((size_t)32*nre,sizeof(*ports));
  double *tx=calloc((size_t)r->n_ant*nre*2,sizeof(*tx));
  uint8_t *occupied=calloc(nre,1);
  if(!r->grid_base || !r->meta || !ports || !tx || !occupied) {
    free(ports);free(tx);free(occupied);nr_csirs_disc_synth_free(r);return -1;
  }
  NR_DL_FRAME_PARMS fp={0};fp.N_RB_DL=r->n_rb;fp.ofdm_symbol_size=nk;fp.symbols_per_slot=14;
  fp.slots_per_frame=cfg->slots_per_frame;
  uint32_t seed=cfg->seed?cfg->seed:1;
  double hr[4],hi[4],slope[4];
  for(int a=0;a<r->n_ant;++a) {
    double phase=6.283185307179586*uniform(&seed), gain=0.85+0.3*uniform(&seed);
    hr[a]=gain*cos(phase);hi[a]=gain*sin(phase);slope[a]=0.08*(uniform(&seed)-0.5);
  }
  const double amp=2048, noise=amp*pow(10,-cfg->snr_db/20)/sqrt(2.0);
  for(int t=0;t<r->capacity;++t) {
    uint32_t abs=cfg->first_slot+t;
    r->meta[t]=(nr_csirs_disc_slot_meta_t){.abs_slot=abs,.frame=(abs/cfg->slots_per_frame)%1024,
      .slot=abs%cfg->slots_per_frame,.valid=1};
    memset(occupied,0,nre);memset(tx,0,(size_t)r->n_ant*nre*2*sizeof(*tx));
    for(int ci=0;ci<cfg->n_resources;++ci) {
      const nr_csirs_disc_synth_resource_t *c=cfg->resources+ci;
      if(abs%c->period!=c->offset) continue;
      memset(ports,0,(size_t)32*nre*sizeof(*ports));
      c16_t *planes[32];for(int p=0;p<32;++p) planes[p]=ports+(size_t)p*nre;
      csi_mapping_parms_t m=get_csi_mapping_parms(c->row,c->fd_bitmap,c->l0,c->l1);
      /* OAI's even-dot5 modulation-length helper floors end/2. With an
       * odd exclusive end it leaves the last occupied RB's sequence samples
       * uninitialized. Include one silent odd RB in the generation request:
       * the RE union is unchanged, even when that odd RB is past the grid. */
      int gen_nrb=c->n_rb+(c->density==0 && ((c->rb_start+c->n_rb)&1));
      /* OAI QPSK table has magnitude 1/sqrt(2); SS offset 2 cancels it. */
      nr_generate_csi_rs(&fp,&m,2048,abs%cfg->slots_per_frame,c->density,c->rb_start,gen_nrb,
                         c->l0,c->l1,c->row,c->scramb_id,2,c->cdm,planes);
      int gs=1<<c->cdm;
      for(int j=0;j<nre;++j) {
        bool active=false;for(int p=0;p<m.ports;++p) active|=planes[p][j].r || planes[p][j].i;
        if(!active) continue;
        occupied[j]=1;
        /* All ports generated. A dominant port per CDM group with weak cross
         * ports is a deterministic flat MIMO channel, not power added on top
         * of PDSCH. Independent RX phase/gain is applied below. */
        for(int a=0;a<r->n_ant;++a) for(int p=0;p<m.ports;++p) {
          double gain=p%gs==0?1.0:0.025;
          double phase=p%gs==0?0:0.7*(p+1)*(a+1), re=gain*cos(phase),im=gain*sin(phase);
          size_t b=((size_t)a*nre+j)*2;
          tx[b]+=sqrt(c->power)*(re*planes[p][j].r-im*planes[p][j].i);
          tx[b+1]+=sqrt(c->power)*(re*planes[p][j].i+im*planes[p][j].r);
        }
      }
    }
    int width=cfg->load==1?r->n_rb:(int)lrint(r->n_rb*cfg->load*(0.5+0.5*uniform(&seed)));
    int start=width<r->n_rb?rng(&seed)%(r->n_rb-width+1):0;
    double slot_amp=amp*(0.8+0.4*uniform(&seed));
    /* SS/PBCH-like 240-subcarrier, four-symbol block once per 20 ms.
     * Structural exclusion is supplied by the caller from known SSB location. */
    int ss_start=(nk-240)/2;
    for(int l=0;l<14;++l) for(int k=0;k<nk;++k) {
      int j=l*nk+k,rb=k/12;
      if(!occupied[j]) {
        bool ssb=cfg->ssb && nk>=240 && abs%(2*cfg->slots_per_frame)==0 && l>=0 && l<4 && k>=ss_start && k<ss_start+240;
        bool scheduled=rb>=start && rb<start+width;
        double a=ssb?amp*sqrt(2):scheduled?slot_amp:0;
        if(!ssb && scheduled && cfg->dmrs && (l==2 || l==11) && k%2==0) a*=sqrt(1.9);
        double re=(rng(&seed)&1?1:-1)*a/sqrt(2),im=(rng(&seed)&1?1:-1)*a/sqrt(2);
        for(int ant=0;ant<r->n_ant;++ant) {size_t b=((size_t)ant*nre+j)*2;tx[b]=re;tx[b+1]=im;}
      }
      for(int ant=0;ant<r->n_ant;++ant) {
        size_t b=((size_t)ant*nre+j)*2;
        double flat=1+slope[ant]*(2.0*k/nk-1),re=tx[b],im=tx[b+1];
        int16_t *dst=nr_csirs_disc_ring_sym(r,t,ant,l)+2*k;
        dst[0]=quantize(flat*(hr[ant]*re-hi[ant]*im)+noise*gaussian(&seed));
        dst[1]=quantize(flat*(hr[ant]*im+hi[ant]*re)+noise*gaussian(&seed));
      }
    }
  }
  r->written=r->capacity;free(ports);free(tx);free(occupied);return 0;
}
