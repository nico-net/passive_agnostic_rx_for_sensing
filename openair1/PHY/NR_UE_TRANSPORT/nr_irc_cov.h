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
#endif
