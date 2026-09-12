/* Anytime Bernoulli confidence bounds for adaptive CRC sampling.
 * Union bound over classes and sample counts: total error <= 1e-6.
 * The service-quality shortcut requires a lower bound of 60%, not a
 * hand-picked configuration or an unvalidated high empirical rate. */
#ifndef NR_CRC_EVIDENCE_H
#define NR_CRC_EVIDENCE_H
#include <math.h>
#include <stdint.h>
static inline double nr_crc_kl(double p, double q)
{
  double d=0;
  if (p>0) d+=p*log(p/q);
  if (p<1) d+=(1-p)*log((1-p)/(1-q));
  return d;
}
static inline void nr_crc_interval(uint64_t passes, uint64_t trials, unsigned classes,
                                   double *lower, double *upper)
{
  *lower=0; *upper=1;
  if (!trials || passes>trials) return;
  const double n=(double)trials, p=(double)passes/n;
  const double b=log(2.0*classes*(n+1)*(n+2)/1e-6)/n;
  if (!passes) { *upper=1-exp(-b); return; }
  if (passes==trials) { *lower=exp(-b); return; }
  double a=0,z=p;
  for(int i=0;i<32;i++) { double m=(a+z)*0.5; if(nr_crc_kl(p,m)>b) a=m; else z=m; }
  *lower=a; a=p; z=1;
  for(int i=0;i<32;i++) { double m=(a+z)*0.5; if(nr_crc_kl(p,m)>b) z=m; else a=m; }
  *upper=z;
}
static inline void nr_crc_shuffle(int *order, int n, uint32_t *state)
{
  if (!*state) *state=UINT32_C(0x6d2b79f5);
  for(int i=n-1;i>0;i--) {
    uint32_t x=*state; x^=x<<13; x^=x>>17; x^=x<<5; *state=x;
    int j=(int)(x%(uint32_t)(i+1)), t=order[i]; order[i]=order[j]; order[j]=t;
  }
}
#endif
