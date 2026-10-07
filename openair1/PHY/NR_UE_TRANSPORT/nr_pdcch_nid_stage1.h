/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#pragma once
#include "nr_pdcch_nid_gpu.h"
#include <math.h>
#ifdef __cplusplus
#include "nr_pdcch_nid_algorithm.h"

// GF(2) 1600-bit jump. After initialization, nid_gold advances 32 bits/step.
#ifdef __CUDACC__
static __device__ __constant__ uint32_t nid_jump[8][16] = {
#else
static const uint32_t nid_jump[8][16] = {
#endif
{0x0u,0x70889900u,0x9199ab01u,0xe1113201u,0x53bbcf03u,0x23335603u,0xc2226402u,0xb2aafd02u,0xd7ff0707u,0xa7779e07u,0x4666ac06u,0x36ee3506u,0x8444c804u,0xf4cc5104u,0x15dd6305u,0x6555fa05u},
{0x0u,0xaffe0e0eu,0x5ffc1c1cu,0xf0021212u,0xbff83838u,0x10063636u,0xe0042424u,0x4ffa2a2au,0x7ff07070u,0xd00e7e7eu,0x200c6c6cu,0x8ff26262u,0xc0084848u,0x6ff64646u,0x9ff45454u,0x300a5a5au},
{0x0u,0xffe0e0e1u,0xffc1c1c2u,0x212123u,0xff838384u,0x636365u,0x424246u,0xffa2a2a7u,0xff070708u,0xe7e7e9u,0xc6c6cau,0xff26262bu,0x84848cu,0xff64646du,0xff45454eu,0xa5a5afu},
{0x0u,0xfe0e0e11u,0xfc1c1c22u,0x2121233u,0xf8383844u,0x6363655u,0x4242466u,0xfa2a2a77u,0xf0707088u,0xe7e7e99u,0xc6c6caau,0xf26262bbu,0x84848ccu,0xf64646ddu,0xf45454eeu,0xa5a5affu},
{0x0u,0xe0e0e111u,0xc1c1c222u,0x21212333u,0x83838444u,0x63636555u,0x42424666u,0xa2a2a777u,0x7070889u,0xe7e7e998u,0xc6c6caabu,0x26262bbau,0x84848ccdu,0x64646ddcu,0x45454eefu,0xa5a5affeu},
{0x0u,0xe0e1113u,0x1c1c2226u,0x12123335u,0x3838444cu,0x3636555fu,0x2424666au,0x2a2a7779u,0x70708899u,0x7e7e998au,0x6c6caabfu,0x6262bbacu,0x4848ccd5u,0x4646ddc6u,0x5454eef3u,0x5a5affe0u},
{0x0u,0xe0e11132u,0xc1c22264u,0x21233356u,0x838444c8u,0x636555fau,0x424666acu,0xa2a7779eu,0x7088990u,0xe7e998a2u,0xc6caabf4u,0x262bbac6u,0x848ccd58u,0x646ddc6au,0x454eef3cu,0xa5affe0eu},
{0x0u,0xe111320u,0x1c222640u,0x12333560u,0x38444c80u,0x36555fa0u,0x24666ac0u,0x2a7779e0u,0x70889900u,0x7e998a20u,0x6caabf40u,0x62bbac60u,0x48ccd580u,0x46ddc6a0u,0x54eef3c0u,0x5affe0e0u}
};
NID_FN inline uint32_t nid_gold_start(uint32_t *x1,uint32_t *x2,uint32_t seed) {
 *x1=0x5e485840u;*x2=0;
#ifdef __CUDACC__
 #pragma unroll
#endif
 for(int b=0;b<8;b++)*x2^=nid_jump[b][(seed>>(4*b))&15];
 return *x1^*x2;
}
// Shared pilot sums/differences are exact integers in binary32, including
// int16 extrema. Sign selection preserves the oracle's per-RE and prefix order.
struct NidSigns { float plus, minus; };
NID_FN inline NidSigns nid_signs(c16_t y) {
 return {(float)((int)y.r+y.i),(float)((int)y.r-y.i)};
}
NID_FN inline uint32_t nid_x2_next(uint32_t x) {
 x=(x>>1)^(x>>2)^(x>>3)^(x>>4);
 return x^(x<<31)^(x<<30)^(x<<29)^(x<<28);
}
NID_FN inline void nid_qpsk(NidSigns y,unsigned bits,float &r,float &i) {
 bool flip=(bits&1)!=0, cross=((bits^(bits>>1))&1)!=0;
 float a=cross?y.minus:y.plus, b=cross?y.plus:-y.minus;
 r+=flip?-a:a; i+=flip?-b:b;
}
// Denominators are computed ONCE per window, in the oracle's power-prefix
// order. Zero also encodes excluded/short windows. No ID-dependent mask loops.
NID_FN inline void nid_denominators(const NidSigns *pilots,float *den,int lo,int hi,int exlo,int exhi) {
 float prefix[6]={},sum=0;
 for(int c=lo;c<hi;c++) {
  float p=0;
  for(int q=0;q<3;q++) {
   NidSigns y=pilots[3*c+q];
   float r=(y.plus+y.minus)*.5f,i=(y.plus-y.minus)*.5f;
   p+=r*r+i*i;
  }
  float old=prefix[1];
#ifdef __CUDACC__
  #pragma unroll
#endif
  for(int j=0;j<5;j++)prefix[j]=prefix[j+1];
  prefix[5]=sum;sum+=p;
  bool valid=c-lo>=5 && !(exhi>exlo && c-5<exhi && c+1>exlo);
  den[c]=valid?(sum-old)*36.f:0;
 }
}
NID_FN inline void nid_max_ratio(float r,float i,float denominator,float &best) {
 if(denominator<=0)return;
 float numerator=r*r+i*i,lower=best*denominator;
#ifdef __CUDA_ARCH__
 if(lower>0)lower=__uint_as_float(__float_as_uint(lower)-1);
#else
 lower=nextafterf(lower,-INFINITY);
#endif
 if(numerator>lower) {float value=numerator/denominator;if(value>best)best=value;}
}
// Three Gold words cover exactly sixteen RBs. Fully unrolled extraction has
// constant shifts; x1 is shared, only x2 advances per ID (no per-RE while/div).
NID_FN inline float nid_best_prepared(uint32_t seed,int lo,int hi,int m0,
                                     const NidSigns *pilots,const float *den,const uint32_t *x1words) {
 uint32_t unused,x2;nid_gold_start(&unused,&x2,seed);
 float ar[6]={},ai[6]={},sr=0,si=0,best=0;
 for(int base=m0;base<hi;base+=16) {
  uint32_t words[3];
#ifdef __CUDACC__
  #pragma unroll
#endif
  for(int w=0;w<3;w++) {words[w]=x2^x1words[3*((base-m0)/16)+w];x2=nid_x2_next(x2);}
#ifdef __CUDACC__
  #pragma unroll
#endif
  for(int t=0;t<16;t++) {
   int c=base+t;if(c<lo || c>=hi)continue;
   float r=0,i=0;
#ifdef __CUDACC__
   #pragma unroll
#endif
   for(int q=0;q<3;q++)nid_qpsk(pilots[3*c+q],words[(3*t+q)/16]>>(2*((3*t+q)%16)),r,i);
   float oldr=ar[1],oldi=ai[1];
#ifdef __CUDACC__
   #pragma unroll
#endif
   for(int j=0;j<5;j++){ar[j]=ar[j+1];ai[j]=ai[j+1];}
   ar[5]=sr;ai[5]=si;sr+=r;si+=i;
   nid_max_ratio(sr-oldr,si-oldi,den[c],best);
  }
 }
 return sqrtf(best);
}
// Stage 2 caches excess in logical bundle order. Candidate summation still
// visits each bundle in the original order (no AL hierarchy/tree rounding).
NID_FN inline double nid_cached_candidate(const double *excess,int first,int count) {
 double sum=0;for(int j=first;j<first+count;j++)sum+=excess[j];
 return count?sum/count:0;
}
// Prefix arithmetic follows the CPU oracle exactly (the old GPU sliding sum
// did not). --fmad=false, precise div/sqrt, no fast-math preserve decisions.
NID_FN inline float nid_best_symbol(const NidView &v,int k,uint32_t seed,int lo,int hi,int exlo,int exhi,int m0,int d=0,const c16_t *pilots=nullptr,const float *power=nullptr) {
 // Keep the last six PREFIX values, not a sliding sum: subtraction and
 // rounding are identical to the CPU's full prefix arrays, with no large spills.
 float ar[6]={},ai[6]={},ap[6]={},sr=0,si=0,sp=0,b=0;
 const c16_t *re=v.grid(k,d);
 // Sequence words are consumed monotonically: no per-thread 54-word local array.
 uint32_t x1=0,x2=seed,word=0;int current=-1;
 for(int c=lo;c<hi;c++) {
  float r=0,i=0,p=0;
  for(int q=0;q<3;q++) {
   int m=3*(c-m0)+q;
   while(current<m/16) {word=current<0?nid_gold_start(&x1,&x2,seed):nid_gold(&x1,&x2,0);current++;}
   // Same conjugated QPSK mapping as idsw_pilot, without a local-memory lookup.
   unsigned bits=(word>>(2*(m%16)))&3;
   float xr=(bits&1)?-1.f:1.f,xi=(bits&2)?1.f:-1.f;
   c16_t y=pilots?pilots[3*c+q]:re[v.point_a_sc+12*c+1+4*q];
   r+=y.r*xr-y.i*xi; i+=y.r*xi+y.i*xr;
   if(!power)p+=(float)y.r*y.r+(float)y.i*y.i;
  }
  int j=c-lo;
#ifdef __CUDACC__
  #pragma unroll
#endif
  for(int t=0;t<5;t++) {ar[t]=ar[t+1];ai[t]=ai[t+1];ap[t]=ap[t+1];}
  ar[5]=sr;ai[5]=si;ap[5]=sp;
  sr+=r;si+=i;sp+=p;
  if(j<5)continue;
  int first=c-5;if(exhi>exlo && first<exhi && first+6>exlo)continue;
  float wr=sr-ar[0],wi=si-ai[0],wp=power?power[c+1]-power[c-5]:sp-ap[0];
  if(wp>0) {
   float numerator=wr*wr+wi*wi,denominator=wp*36.f;
   // A lower adjacent float bounds the exact product b*denominator.
   // Below it the rounded quotient cannot exceed b, so most divisions
   // can be skipped without changing even a one-ULP decision.
   float lower=b*denominator;
#ifdef __CUDA_ARCH__
   if(lower>0)lower=__uint_as_float(__float_as_uint(lower)-1);
#else
   lower=nextafterf(lower,-INFINITY);
#endif
   if(numerator>lower) {
    float value=numerator/denominator;
    if(value>b)b=value;
   }
  }
 }
 return sqrtf(b);
}

#endif // __cplusplus
#ifdef __CUDACC__
#define NID_COMMON __host__ __device__
#else
#define NID_COMMON
#endif
#define NID_IDS 65536
#define NID_CELLS (14 * NID_IDS)
#define NID_CAL 1024
/* Secondary symbols use a fixed, uniformly spaced null panel, independent of
 * the first-symbol screen. No selected-ID bias in median/MAD calibration.
 * The sum/sqrt(D) statistic requires z > 2 on EVERY constituent symbol. This
 * both rejects padded durations and lets us skip secondary Gold sweeps for
 * >95% of IDs. Lambda is measured AFTER this gate, separately for each D/sym.
 * See T19.md for the family-wise Poisson model and its limitations. */
NID_COMMON static inline double nid_poisson_tail(double lambda, int k)
{
  if (lambda == 0) return 0;
  if (k <= lambda) return 1; /* only upper tails are needed by the threshold */
  double term = exp(-lambda + k * log(lambda) - lgamma(k + 1.0)), sum = term;
  for (int j = k + 1; j < k + 10000; j++) {
    term *= lambda / j;
    sum += term;
    if (term < sum * 1e-15) break;
  }
  return sum;
}
NID_COMMON static inline int nid_threshold_alpha(double lambda, int families, double alpha)
{
  int k = 1;
  while (k < 65536 && families * (double)NID_IDS * nid_poisson_tail(lambda, k) >= alpha) k++;
  return k;
}
NID_COMMON static inline int nid_threshold(double lambda, int families) {
 return nid_threshold_alpha(lambda, families, 0.01);
}
/* Spending across every look, including ring wraps: sum 1/(t(t+1)) = 1.
 * Same Poisson null model as T19, now a 1% lifetime family-wise budget. */
NID_COMMON static inline double nid_look_alpha(uint64_t look) {
 return 0.01 / ((double)look * ((double)look + 1.0));
}
NID_COMMON static inline void nid_list_insert(nr_pdcch_nid_result_t *out,int id,int s,int d,float strength) {
        bool dominated = false;
        for (int i = 0; i < out->list_n; i++)
          if (out->list_id[i] == id && s < out->list_sym[i]+out->list_dur[i]
              && out->list_sym[i] < s+d+1 && strength <= out->list_score[i]) dominated=true;
        if (dominated) return;
        // A longer interval may cover several previously selected subwindows.
        for (int i=0; i<out->list_n;) {
          if (out->list_id[i]!=id || s>=out->list_sym[i]+out->list_dur[i] || out->list_sym[i]>=s+d+1) {i++;continue;}
          for (int j=i; j+1<out->list_n; j++) {
            out->list_id[j]=out->list_id[j+1];out->list_sym[j]=out->list_sym[j+1];
            out->list_dur[j]=out->list_dur[j+1];out->list_score[j]=out->list_score[j+1];
          }
          out->list_n--;
        }
        int at=0;
        while (at<out->list_n && out->list_score[at]>=strength) at++;
        if (at>=40) return;
        if (out->list_n<40) out->list_n++;
        for (int i=out->list_n-1;i>at;i--) {
          out->list_id[i]=out->list_id[i-1];out->list_sym[i]=out->list_sym[i-1];
          out->list_dur[i]=out->list_dur[i-1];out->list_score[i]=out->list_score[i-1];
        }
        out->list_id[at]=id;out->list_sym[at]=s;out->list_dur[at]=d+1;out->list_score[at]=strength;
}
NID_COMMON static inline void nid_shortlist(const uint16_t *hits, const float *zs, int duration,
                                       nr_pdcch_nid_result_t *out)
{
  int families = 0;
  for (int d = 1; d <= duration; d++) families += 15 - d;
  out->list_n = 0;
  for (int r = 0; r < 2; r++) {
    float best = -1;
    out->hit_hash[r] = 1469598103934665603ULL;
    for (int d = 0; d < duration; d++) for (int s = 0; s < 14-d; s++) {
      int base = (2*d+r)*NID_CELLS+s*NID_IDS;
      double total = 0;
      for (int id = 0; id < NID_IDS; id++) {
        total += hits[base+id];
        out->hit_hash[r] = (out->hit_hash[r]^hits[base+id])*1099511628211ULL;
        if (!d) {
          float v = hits[base+id]+zs[base+id]*1e-4f;
          if (v > best) {best=v;out->top_id[r]=id;out->top_sym[r]=s;}
        }
      }
      double lambda = total / NID_IDS;
      int threshold = nid_threshold(lambda, families);
      if (!r) continue;
      out->lambda[d][s] = lambda;
      out->threshold[d][s] = threshold;
      for (int id = 0; id < NID_IDS; id++) {
        int h = hits[base+id];
        if (h < threshold) continue;
        /* Score of accepted occasions; coherent evidence across more symbols
         * wins over its strict subwindows. Stable scan order breaks ties. */
        float strength = zs[base+id];
        nid_list_insert(out,id,s,d,strength);
      }
    }
  }
}
