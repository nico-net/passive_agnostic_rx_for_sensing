/* Offline measurement only. No decisions, grants, or configuration are published.
 * Included after replay_header_t by nr_passive_replay_capture.c. Correlations use
 * raw pilot REs, NOT interpolated channel estimates (which manufacture smoothness).
 */
#include "nr_pusch_passive_decode.h"
#include "PHY/NR_REFSIG/nr_refsig.h"

static void replay_load_slot(PHY_VARS_NR_UE *ue, const replay_header_t *h,
                             const unsigned char *samples, unsigned index)
{
  const NR_DL_FRAME_PARMS *fp=&h->fp;
  unsigned frame=index/fp->slots_per_frame, slot=index%fp->slots_per_frame;
  unsigned end=get_samples_slot_timestamp(fp,slot)+get_samples_per_slot(slot,fp);
  for (unsigned a=0;a<fp->nb_antennas_rx;++a) {
    size_t prior=((frame-1)*fp->nb_antennas_rx+a)*fp->samples_per_frame*sizeof(c16_t);
    size_t current=(frame*fp->nb_antennas_rx+a)*fp->samples_per_frame*sizeof(c16_t);
    memcpy(ue->common_vars.rxdata[a],samples+prior,fp->samples_per_frame*sizeof(c16_t));
    memcpy(ue->common_vars.rxdata[a],samples+current,end*sizeof(c16_t));
  }
}
static void replay_probe_ul(PHY_VARS_NR_UE *ue, const replay_header_t *h, const unsigned char *samples)
{
  const NR_DL_FRAME_PARMS *fp=&h->fp;
  if (fp->N_RB_UL<1 || fp->N_RB_UL>275 || !fp->ofdm_offset_divisor) {
    printf("UL-PILOT VOID: invalid FFT geometry\n"); return;
  }
  const int N=fp->ofdm_symbol_size, nr=fp->nb_antennas_rx;
  c16_t (*grid)[N]=aligned_alloc(32,(size_t)nr*N*sizeof(c16_t));
  c16_t pilot[275*6] __attribute__((aligned(32)));
  if (!grid) return;
  /* Standards-defined common TA offset; residual per-UE TA is still unknown.
   * This is a diagnostic starting window, not a learned timing identity. */
  const int ta=(int)((uint64_t)25600*fp->samples_per_subframe/(4096ull*480ull));
  printf("UL-PILOT: raw-RE lag-one coherence, 12-RB windows; PCI and wrong-PCI controls; no config decisions\n");
  for (unsigned index=fp->slots_per_frame;index<h->slots;++index) {
    replay_load_slot(ue,h,samples,index);
    const int slot=index%fp->slots_per_frame;
    for (int sym=0;sym<fp->symbols_per_slot;++sym) {
      for (int a=0;a<nr;++a)
        nr_pusch_passive_fep_symbol(fp,(c16_t *)ue->common_vars.rxdata[a],grid[a],sym,slot,ta,h->slot[index].fo);
      double peak[2]={0,0}; int best_rb[2]={0,0}, best_scid[2]={0,0}, best_group[2]={0,0};
      for (int control=0;control<2;++control)
        for (int scid=0;scid<2;++scid) {
          const int nid=(fp->Nid_cell+control)%1008;
          const uint32_t *gold=nr_gold_pusch(fp->N_RB_UL,fp->symbols_per_slot,nid,scid,slot,sym);
          nr_pusch_dmrs_rx(fp->Ncp,gold,pilot,1000,0,fp->N_RB_UL,0,0,16384);
          for (int group=0;group<2;++group) {
            double cr[4][276]={{0}},ci[4][276]={{0}},ep[4][276]={{0}},en[4][276]={{0}};
            for (int a=0;a<nr;++a)
              for (int rb=0;rb<fp->N_RB_UL;++rb) {
                double pr=0,pi=0,rr=0,ii=0,e0=0,e1=0;
                for (int k=0;k<6;++k) {
                  const c16_t y=grid[a][(fp->first_carrier_offset+12*rb+2*k+group)%N];
                  const c16_t q=pilot[6*rb+k]; /* Already conjugated by production generator. */
                  double xr=((double)y.r*q.r-(double)y.i*q.i)/32768.0;
                  double xi=((double)y.r*q.i+(double)y.i*q.r)/32768.0;
                  if (k) {
                    rr+=xr*pr+xi*pi; ii+=xi*pr-xr*pi;
                    e0+=pr*pr+pi*pi; e1+=xr*xr+xi*xi;
                  }
                  pr=xr; pi=xi;
                }
                cr[a][rb+1]=cr[a][rb]+rr; ci[a][rb+1]=ci[a][rb]+ii;
                ep[a][rb+1]=ep[a][rb]+e0; en[a][rb+1]=en[a][rb]+e1;
              }
            for (int rb=0;rb+12<=fp->N_RB_UL;++rb) {
              double numerator=0,denominator=0;
              for (int a=0;a<nr;++a) {
                double re=cr[a][rb+12]-cr[a][rb],im=ci[a][rb+12]-ci[a][rb];
                numerator+=hypot(re,im);
                denominator+=sqrt((ep[a][rb+12]-ep[a][rb])*(en[a][rb+12]-en[a][rb]));
              }
              double rho=denominator>0?numerator/denominator:0;
              if (rho>peak[control]) {
                peak[control]=rho; best_rb[control]=rb; best_scid[control]=scid; best_group[control]=group;
              }
            }
          }
        }
      printf("UL-PILOT source=%ld slot=%d sym=%d rho=%.5f rb=%d nscid=%d group=%d wrong=%.5f wrong_rb=%d\n",
             h->slot[index].source,slot,sym,peak[0],best_rb[0],best_scid[0],best_group[0],peak[1],best_rb[1]);
    }
  }
  free(grid);
}
