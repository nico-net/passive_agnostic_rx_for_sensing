/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_SENSE_UL_H
#define NR_SENSE_UL_H
#include <math.h>
/* Express H at the unadvanced DL-frame FFT window, after RX symbol/CP rotation.
 * The UL FEP reads 'advance' samples earlier. Undo that window's phase ramp.
 * This affects only exported copies, never the decoder's channel estimates. */
static inline void nr_sense_ul_to_dl(float *re,float *im,int k_crb,int first_carrier_offset,int fft_size,int advance)
{
  double phase=2*M_PI*((first_carrier_offset+k_crb)%fft_size)*advance/fft_size;
  double r=*re,i=*im;
  *re=r*cos(phase)-i*sin(phase);*im=r*sin(phase)+i*cos(phase);
}
#endif
