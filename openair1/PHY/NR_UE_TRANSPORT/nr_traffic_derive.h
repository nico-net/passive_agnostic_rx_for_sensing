/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/* Traffic mode (E7): fields derived from a PROVEN DCI layout instead of read from a decode: layers (nl), modulation order (qm), transport block size (tbs).
 * Every function is pure and answers "unknown" (-1 / 0 / false) unless every input it needs is proven; the caller states the proofs, nothing is guessed. */
#ifndef NR_TRAFFIC_DERIVE_H
#define NR_TRAFFIC_DERIVE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Qm and code rate (R x 1024 x 10, the nr_compute_tbs unit) of an MCS index under an MCS table. dir 0 = DL (TS 38.214 Tables 5.1.3.1-1/-2/-3 = table 0..2),
 * dir 1 = UL (the same, 3..5 = the transform-precoding variants of TS 38.214 6.1.4.1). false for an unknown table, an MCS above 31 or a RESERVED index
 * (retransmission codepoints: their Qm/TBS follow the initial transmission, so they are never read as a value). */
bool nr_traffic_mcs_qr(int dir, int table, int mcs, int *qm, uint32_t *r10);

/* TS 38.214 5.1.3.2 TBS in BITS of a single-codeword PDSCH/PUSCH. 0 (unknown) unless: qm/r10 valid, 1 <= nl <= 4 (more layers split over two codewords: not derived),
 * xoh in {0,6,12,18} (xOverhead PROVEN, -1 = not proven), n_cdm 1..3, dmrs_syms >= 1 and the DM-RS REs fit. dmrs_type 0 = type 1 (6 REs per CDM group and symbol), 1 = type 2 (4). */
uint32_t nr_traffic_tbs(int qm, uint32_t r10, int nb_rb, int nb_sym, int dmrs_type, int n_cdm, int dmrs_syms, int xoh, int nl);

/* Inputs of the DL 1_1 derivation: each member is a proof the caller took, -1 / false = not proven. */
typedef struct {
  bool layout_trusted;      /* the layout behind the fields is CRC-solved, or PHYS-solved with trusted fields (NOT unproven) */
  bool layout_crc;          /* the layout was solved by a full TB CRC pass (S, L and the tbs inputs may then be read) */
  int ap_bits, dmrs_type;   /* the layout's antenna-ports width and DM-RS type (0 = type 1) */
  int ap_field;             /* the antenna-ports codepoint read with that layout; -1 unknown */
  uint8_t rd_cdm; uint16_t rd_ports;   /* the reading's own CDM groups / port bitmask: must equal the table lookup */
  int mcs, mcs_table;       /* the reading's MCS and MCS table */
  int table_evidence;       /* nr_pdsch_solver_table_evidence(): 0 none, 1 CRC pass, 2 Qm consensus */
  bool sv_ok, sv_crc, sv_ov;   /* nr_pdsch_solver_derive_inputs() */
  int sv_add_pos, sv_table, sv_S, sv_L;
  int nb_rb, S, L; uint16_t dmrs_mask;   /* the reading's allocation and DM-RS symbol mask */
  uint16_t expect_mask;     /* nr_pdsch_solver_dmrs_mask(first DM-RS symbol, S, L, sv_add_pos), 0 = not computed */
  int xoh;                  /* xOverhead proven by the cell's CRC-confirmed elimination, -1 not proven */
  int k0;                   /* PDSCH slot offset from the DCI slot, from a LEARNT TDA row; -1 unknown (the tree learns no per-index k0 for DCI 1_1) */
  int slot_dl_syms;         /* nr_passive_acq_tdd_pdsch_dl_symbols() of slot (dci slot + k0): 14 = full DL slot; -1 pattern unknown. Anything but 14 (special, flexible, UL) = no tbs */
  bool class_c;             /* C-RNTI grant (tb scaling 0, xOverhead applies) */
} nr_traffic_dl_in_t;
typedef struct { int8_t nl, qm; int32_t tbs; } nr_traffic_derived_t;   /* -1 unknown */
void nr_traffic_derive_dl(const nr_traffic_dl_in_t *in, nr_traffic_derived_t *out);

#ifdef __cplusplus
}
#endif
#endif
