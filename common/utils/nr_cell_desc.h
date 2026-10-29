/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
#ifndef NR_CELL_DESC_H
#define NR_CELL_DESC_H
/* Cell descriptor: what the blind front end (cells.json, plan B.7) tells the receiver about one cell. Plain C, no dependencies: the seam shared by the admission
 * model (nr_cell_admit.h) and the cell manager (D1). Nothing here is a host value; every field is a property of the cell or of the capture window.
 *
 * Unknowns are explicit: pci = -1, n_rb = 0, n_rx = 0, scs_khz = 0, window_centre_hz = 0.
 *   n_rb == 0 (grid not yet known) is SIZED as the largest grid of its SCS (3GPP TS 38.101-1/-2 maximum channel bandwidth: 15 kHz 270, 30 kHz 273, 60 kHz 135,
 *   120 kHz 264), so an unknown cell never gets a smaller buffer than its real grid needs. n_rx == 0 -> 4 (the build's maximum). scs_khz == 0 -> 30.
 *   A cell needs the channelizer unless it sits at the window centre: zero shift iff window_centre_hz != 0 && centre_hz == window_centre_hz. */
#ifdef __cplusplus
extern "C" {
#endif
#ifndef NR_CELL_MAX
#define NR_CELL_MAX 16 /* cell slots of one process (plan B.2); A1 defines the same value in nr_cell.h */
#endif
/* Lifecycle state as the inventory reports it. Admission order: MAINTAIN (already served, never displaced by a newcomer), ACQUIRE, NEW; LOST is not admitted. */
typedef enum { NR_CELLD_NEW = 0, NR_CELLD_ACQUIRE = 1, NR_CELLD_MAINTAIN = 2, NR_CELLD_LOST = 3 } nr_cell_desc_state_t;
typedef struct {
  double centre_hz;        /* receiver -C of this cell */
  double ssb_hz;           /* SSB centre frequency (0 unknown) */
  int gscn;                /* global synchronisation channel number (0 unknown) */
  int ssb_index;           /* SSB beam index (-1 unknown) */
  int pci;                 /* physical cell id, -1 unknown */
  double cfo_hz;           /* carrier offset seed */
  int n_rb;                /* grid width in PRB, 0 unknown (see above) */
  int scs_khz;             /* 15 / 30 / 60 / 120, 0 unknown */
  int n_rx;                /* RX antennas of this cell's stream, 0 unknown */
  int window;              /* id of the capture window (one radio claim) the cell sits in */
  double window_centre_hz; /* centre of that window, 0 unknown */
  int weight;              /* share of the scalable budgets among served cells; <= 0 means 1 */
  int state;               /* nr_cell_desc_state_t */
  int placeholder;         /* B6: a spare slot's descriptor (nr_cell_life): prepared like a cell, runs nothing, so it carries no CPU demand */
} nr_cell_desc_t;
static inline void nr_cell_desc_init(nr_cell_desc_t *c)
{
  *c = (nr_cell_desc_t){.ssb_index = -1, .pci = -1, .weight = 1, .state = NR_CELLD_NEW};
}
#ifdef __cplusplus
}
#endif
#endif
