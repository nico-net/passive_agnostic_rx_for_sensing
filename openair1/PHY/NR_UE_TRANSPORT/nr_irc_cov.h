/* T26d ISAC_RX_IRC=4: per-grant interference+noise covariance from the PDSCH DM-RS residuals, per 4-RB group,
 * written by the passive decode after channel estimation and read by nr_rx_pdsch()'s whitening IRC (same thread). */
#ifndef NR_IRC_COV_H
#define NR_IRC_COV_H
#include <complex.h>
#include <stdint.h>
#define NR_IRC_MAX_GROUPS 72
typedef struct {
  int valid, ng, nb;
  double complex R[NR_IRC_MAX_GROUPS][4][4];
  uint32_t n[NR_IRC_MAX_GROUPS];
} nr_irc_cov_t;
extern __thread nr_irc_cov_t nr_irc_dmrs_cov;

/* D4 IRC (2026-10-31): the grant's DM-RS interference whitening, per RB group (nr_mmse_exact.h: nr_irc_final), written by the passive decode after the channel
 * estimate (nr_pdsch_irc_cov(), CPU chest hook) and read on the same thread by the mode-3 MMSE (nr_dlsch_mmse) and by the GPU demod job (nr_pdsch_gpu_demod_host.c). */
#include "nr_mmse_exact.h"
typedef struct {
  int valid;        /* 1: w[] are usable for the grant being demodulated (nb RX branches, ng groups) */
  int nb, ng, nre; /* RX branches, groups, REs of a full symbol of the grant (nr_irc_group_of) */
  int active;       /* groups whose covariance was usable (the others are identity = scalar noise model) */
  double gain_db;   /* mean whitening gain estimate over the active groups (10 log10 tr(R^-1) tr(R) / nb^2): what a random signal direction gains over MRC */
  double w[NR_IRC_NG_MAX * NR_IRC_WSTRIDE];
} nr_irc_grant_t;
extern __thread nr_irc_grant_t nr_irc_grant;
#endif
