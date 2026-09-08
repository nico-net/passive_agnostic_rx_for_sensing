/* Isolated offline contract test; no modem, RF, runtime flags or repository edits.
 * Actual production FEP/FFT/equalizer; known transmitted pilots supply H.
 * eps > 0 means receiver samples at transmitter time (1-eps)*n/Fs.
 * Estimator/rotation below mirror production formulas, not production dispatch. */
#include "PHY/defs_nr_UE.h"
#include "PHY/MODULATION/modulation_UE.h"
#include "PHY/TOOLS/tools_defs.h"
#include "nr_channel_compensation.h"
#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void dft_implementation(uint8_t, int16_t *, int16_t *, unsigned char);
dftfunc_t dft = dft_implementation;
enum {N=512,L=96,STRIDE=N*14};
static NR_DL_FRAME_PARMS fp;
static PHY_VARS_NR_UE *ue;
static c16_t *samples;
static c16_t bins[1][STRIDE] __attribute__((aligned(32)));
static double complex ideal[3][L], obs[3][L];
static const int syms[3]={2,11,13};
static const double fs=15360000.0;
static unsigned off(int m) { return 40+548*m-4; }
static double complex z(c16_t x) { return x.r + I*x.i; }
static c16_t quant(double complex x) {
  if (fabs(creal(x)) > 32000 || fabs(cimag(x)) > 32000) { fprintf(stderr,"VOID: clipping\n"); abort(); }
  return (c16_t){lrint(creal(x)),lrint(cimag(x))};
}
static int ready;
static void init(void) {
  if(ready) return;
  ready=1; InitSinLUT();
  ue=calloc(1,sizeof(*ue)); samples=calloc(2*153600,sizeof(*samples));
  if(!ue || !samples) abort();
  ue->is_synchronized=true;
  fp=(NR_DL_FRAME_PARMS){.ofdm_symbol_size=N,.nb_prefix_samples=36,.nb_prefix_samples0=40,
    .first_carrier_offset=368,.symbols_per_slot=14,.slots_per_subframe=2,.slots_per_frame=20,
    .samples_per_subframe=15360,.samples_per_slot0=7680,.samples_per_slotN0=7680,
    .samples_per_frame=153600,.samples_per_slot_wCP=STRIDE,.numerology_index=1,
    .nb_antennas_rx=1,.N_RB_DL=24,.subcarrier_spacing=30000,.ofdm_offset_divisor=8};
  for(int j=0;j<3;j++) for(int m=0;m<224;m++) fp.symbol_rotation[j][m]=(c16_t){32767,0};
  for(int j=0;j<8192;j++) fp.timeshift_symbol_rotation[j]=(c16_t){32767,0};
}
/* Direct analytic OFDM generation, independent of the OAI inverse FFT.
 * Cyclic-prefix window and absolute sampling-time drift are retained. */
static void receive(int k0,double f,double ppm,double command,double complex out[3][L]) {
  init(); memset(bins,0,sizeof(bins));
  for(int s=0;s<3;s++) {
    const unsigned start=off(syms[s]), useful=start+4;
    for(unsigned n=start;n<start+N;n++) {
      double complex sum=0;
      for(int j=0;j<L;j++) {
        int k=k0+j;
        /* Deterministic constant-modulus QPSK, not all-identical tones. */
        unsigned h=(unsigned)(j+1)*2654435761u;
        double complex q=((h&0x10000)?1:-1)+I*((h&0x400000)?1:-1);
        double phase=2*M_PI*(k*((double)n-useful-ppm*1e-6*n)/N+f*n/fs);
        sum+=45.0*q*cexp(I*phase);
      }
      samples[n]=quant(sum);
    }
    c16_t *rx[]={samples};
    nr_ue_set_branch_fo_hz(0,command);
    if(nr_slot_fep_ant(ue,&fp,0,syms[s],0,bins,link_type_dl,0,rx)) abort();
    for(int j=0;j<L;j++) out[s][j]=z(bins[0][syms[s]*N+((k0+j+N)%N)]);
  }
  nr_ue_set_branch_fo_hz(0,0);
}
static double dt(void) {return (off(syms[1])-off(syms[0]))/fs;}
static double measured_cfo(double complex a[3][L]) {
  double complex sum=0;
  for(int j=0;j<L;j++) sum+=a[1][j]*conj(a[0][j]);
  return carg(sum)/(2*M_PI*dt());
}
static int baseline(int k0) {
  receive(k0,0,0,0,ideal);
  double phase=0,ampmin=1e9,ampmax=0;
  for(int j=0;j<L;j++) {
    phase=fmax(phase,cabs(ideal[1][j]-ideal[0][j]));
    ampmin=fmin(ampmin,cabs(ideal[0][j])); ampmax=fmax(ampmax,cabs(ideal[0][j]));
  }
  if(phase>0 || ampmin<100 || ampmax>4000 || ampmax/ampmin>1.02) {
    printf("VOID baseline k0=%d mismatch=%g amplitude=%g..%g\n",k0,phase,ampmin,ampmax); return 1;
  }
  return 0;
}
int offline_cfo(void) {
  if(baseline(36)) return 1;
  int fails=0;
  for(int i=0;i<4;i++) {
    double f=(double[]){37,-37,300,-300}[i],m[3];
    double cmd[3]={0,-f,f};
    for(int a=0;a<3;a++) {receive(36,f,0,cmd[a],obs);m[a]=measured_cfo(obs);}
    printf("CFO injected=%+.0f Hz: off=%+.3f current-negative=%+.3f positive-hook=%+.3f Hz\n",f,m[0],m[1],m[2]);
    fails += fabs(m[0]-f)>4 || fabs(m[1]-2*f)>4 || fabs(m[2])>4;
  }
  return fails;
}
static void estimate(double *cfo,double *ppm) {
  double complex sum[2]={0,0};
  for(int j=0;j<L;j++) sum[j/(L/2)]+=obs[1][j]*conj(obs[0][j]);
  double delta=carg(sum[1]*conj(sum[0]));
  /* Same split-sum estimator as production; exact dt separates CP approximation. */
  *cfo=carg(sum[0]+sum[1])/(2*M_PI*dt());
  *ppm=-delta/(L/2)/(2*M_PI*30000*dt())*1e6;
}
static double evm(int k0,double ppm,int mode) {
  c16_t rx[1][L] __attribute__((aligned(32)));
  c16_t ch[1][1][L] __attribute__((aligned(32)));
  c16_t ma[1][L] __attribute__((aligned(32))), mb[1][L] __attribute__((aligned(32))),mc[1][L] __attribute__((aligned(32)));
  c16_t out[L] __attribute__((aligned(32))); c16_t *outs[]={out};
  /* Pilot at symbol 2, data at symbol 13. Only frequency origin/sign changes. */
  double time=(off(syms[2])-off(syms[0]))/fs;
  for(int j=0;j<L;j++) {
    double complex h=obs[0][j]/ideal[0][j];
    int k=mode==1 ? j : k0+j;
    double a=mode==0?0:-2*M_PI*30000*ppm*1e-6*time*k;
    if(mode==3) a=-a;
    ch[0][0][j]=quant(8192*h*cexp(I*a)); rx[0][j]=quant(obs[2][j]);
  }
  nr_channel_compensation(L,L,1,1,rx,ch,ma,mb,mc,outs,NULL,2,0,13);
  double error=0,power=0;
  for(int j=0;j<L;j++){double complex e=z(out[j])-ideal[2][j];error+=pow(cabs(e),2);power+=pow(cabs(ideal[2][j]),2);}
  return 100*sqrt(error/power);
}
int offline_sfo(void) {
  int fails=0;
  for(int alloc=0;alloc<2;alloc++) {
    int k0=alloc?36:-132;
    if(baseline(k0)) return 1;
    receive(k0,0,0,0,obs);
    double ctl=evm(k0,0,0);
    if(ctl>0.25) {printf("VOID equalizer control EVM=%g%%\n",ctl);return 1;}
    printf("SFO no-impairment control k0=%d EVM=%.4f%%\n",k0,ctl);
    for(int p=0;p<4;p++) {
      double ppm=(double[]){2.2,-2.2,20,-20}[p],cfo,est;
      receive(k0,0,ppm,0,obs);estimate(&cfo,&est);
      double a=evm(k0,est,0),b=evm(k0,est,1),c=evm(k0,est,2),d=evm(k0,est,3);
      double expected_cfo=-30000*ppm*1e-6*(k0+(L-1)/2.0);
      printf("SFO injected=%+.1f ppm k0=%d: estimate=%+.4f ppm, apparent-CFO=%+.3f expected=%+.3f Hz; EVM off=%.3f packed=%.3f physical=%.3f reversed=%.3f %%\n",
        ppm,k0,est,cfo,expected_cfo,a,b,c,d);
      fails += fabs(est-ppm)>0.25 || fabs(cfo-expected_cfo)>2 || !(c<a && c<b && c<d);
    }
  }
  return fails;
}

int offline_feedback(void) {
  if(baseline(36)) return 1;
  int fails=0;
  for(int arm=0;arm<3;arm++) {
    double ema=0,command=0,first=0,last=0;
    for(int n=0;n<500;n++) {
      receive(36,37,0,command,obs);
      double residual=measured_cfo(obs);
      if(!n) first=residual;
      last=residual;
      ema=n?0.99*ema+0.01*residual:residual;
      if(arm==0) command=-ema; /* historical wrong-sign law */
      if(arm==1) command=ema; /* sign-only change */
      if(arm==2) command+=0.01*residual; /* independent correction state, reference */
    }
    printf("CFO feedback %s: initial=%+.3f final=%+.3f Hz after 500 updates, command=%+.3f Hz\n",
      (const char*[]){"historical-wrong-sign","sign-only","accumulated-reference"}[arm],first,last,command);
    fails += (arm==0 && last<200) || (arm==1 && (last<14 || last>23)) || (arm==2 && fabs(last)>4);
  }
  return fails;
}
int offline_joint(void) {
  int fails=0;
  for(int alloc=0;alloc<2;alloc++) {
    int k0=alloc?36:-132;
    if(baseline(k0)) return 1;
    for(int sign=0;sign<2;sign++) {
      double f=sign?-37:37,ppm=sign?-2.2:2.2;
      receive(k0,f,ppm,0,obs);
      double cfo,est;estimate(&cfo,&est);
      double intercept=cfo+30000*est*1e-6*(k0+(L-1)/2.0);
      printf("JOINT k0=%d injected-CFO=%+.0f Hz SFO=%+.1f ppm: raw-CFO=%+.3f Hz SFO=%+.4f ppm fitted-origin-CFO=%+.3f Hz\n",
        k0,f,ppm,cfo,est,intercept);
      fails += fabs(est-ppm)>0.25 || fabs(intercept-f)>2;
    }
  }
  return fails;
}


#include <pthread.h>
typedef struct { double snapshot; int use_snapshot; int tls_restored; c16_t bins[1][STRIDE] __attribute__((aligned(32))); } snapshot_job_t;
static void *snapshot_worker(void *arg) {
  snapshot_job_t *job=arg;
  c16_t *rx[1]={samples};
  nr_slot_fep_fo_override_hz=-777.0; /* A reused worker's unrelated prior job. */
  for(int i=0;i<3;i++) {
    if(job->use_snapshot)
      nr_slot_fep_ant_snapshot(ue,&fp,0,syms[i],0,job->bins,link_type_dl,0,rx,job->snapshot);
    else
      nr_slot_fep_ant(ue,&fp,0,syms[i],0,job->bins,link_type_dl,0,rx);
  }
  job->tls_restored=nr_slot_fep_fo_override_hz==-777.0;
  return NULL;
}
int offline_snapshot(void) {
  receive(36,37,0,0,obs); /* Known analytic OFDM, CFO +37 Hz. */
  ue->cont_fo_comp=1; ue->freq_offset=1500; ue->dl_Doppler_shift=0;
  snapshot_job_t *good=aligned_alloc(32,sizeof(*good)+32);
  snapshot_job_t *bad=aligned_alloc(32,sizeof(*bad)+32);
  if(!good || !bad) abort();
  memset(good,0,sizeof(*good));memset(bad,0,sizeof(*bad));
  good->snapshot=37;good->use_snapshot=1;
  pthread_t thread;
  if(pthread_create(&thread,NULL,snapshot_worker,good)) abort();
  pthread_join(thread,NULL);
  if(pthread_create(&thread,NULL,snapshot_worker,bad)) abort();
  pthread_join(thread,NULL);
  double complex a[3][L],b[3][L];
  for(int i=0;i<3;i++) for(int j=0;j<L;j++) {
    a[i][j]=z(good->bins[0][syms[i]*N+36+j]);
    b[i][j]=z(bad->bins[0][syms[i]*N+36+j]);
  }
  double corrected=measured_cfo(a),stale=measured_cfo(b);
  printf("Cross-thread actual FEP: snapshot residual=%+.3f Hz; lost snapshot=%+.3f Hz; worker TLS restored=%d\n",
    corrected,stale,good->tls_restored);
  int fail=fabs(corrected)>4 || fabs(stale-814)>6 || !good->tls_restored;
  free(good);free(bad);ue->cont_fo_comp=0;nr_slot_fep_fo_override_hz=NAN;
  return fail;
}
