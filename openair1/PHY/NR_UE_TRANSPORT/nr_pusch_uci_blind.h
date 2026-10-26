/* Blind UCI-on-PUSCH demultiplexing for the passive receiver (default behaviour, no configuration).
 *
 * A UE whose PUCCH (HARQ-ACK, CSI) overlaps its PUSCH multiplexes that UCI into the PUSCH (TS 38.213 9.2.5, TS 38.212
 * 6.2.7 / 6.3.2.4): HARQ-ACK of more than 2 bits and CSI part 1 / part 2 are RATE-MATCHED (the UL-SCH gets fewer
 * coded bits, every data bit after the first UCI RE moves), HARQ-ACK of 0..2 bits PUNCTURES reserved REs. A receiver
 * that ignores it fails every code block of the grant. The footprint follows from the spec formulas
 *     Q' = min( ceil((O + L) * beta * sum M_UCI / sum K_r), ceil(alpha * sum M_UCI(l >= l0)) [- earlier parts] )
 * with O (payload size), beta (TS 38.213 tables 9.3-1 / 9.3-2) and alpha unknown to a passive receiver; they are searched
 * as hypotheses decided by LDPC code-block CRCs and the TB CRC, and learned per RNTI (O_ACK varies per slot; the CSI
 * report size and the beta offsets / alpha are the UE's configuration and repeat).
 *
 * Search (bounded, per failing grant):
 *   1. the hypotheses learned for this RNTI, re-evaluated for this grant's geometry (the formula transfers across widths,
 *      MCS and symbol counts), screened on code block 0 and confirmed by the TB CRC;
 *   2. when the caller's rate limiter allows, an exploration: the number U of UCI REs is first bounded by the LAST code
 *      block, which (UCI sits in the first symbols of a multi-block grant) depends on U only through its rate-matching
 *      length; then CSI-part hypotheses consistent with U are screened on code block 0 and the HARQ-ACK hypotheses
 *      consistent with the remainder confirmed by the TB CRC.
 * Single-layer and multi-layer codewords (N_L * Q_m bits per RE), DM-RS type 1/2, no PT-RS (the receiver's scope). */
#ifndef NR_PUSCH_UCI_BLIND_H
#define NR_PUSCH_UCI_BLIND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation geometry of the PUSCH (what the UCI mapping depends on). */
typedef struct {
  uint16_t rb;               /* allocated PRBs */
  uint8_t start_sym, nsym;   /* PUSCH symbols [start_sym, start_sym + nsym) */
  uint16_t dmrs_pos;         /* DM-RS symbol bitmap (absolute symbol index) */
  uint8_t data_re_dmrs_rb;   /* UL-SCH REs per PRB on a DM-RS symbol (12 - DM-RS REs incl. CDM groups without data) */
  uint8_t qm, nl;
  uint32_t sum_k;            /* sum of the UL-SCH code block sizes K_r (C * K) */
} nr_uci_geom_t;

/* One UCI hypothesis. o_csi1 == 0: no CSI; o_ack <= 2 with CSI or ACK present: REs reserved with O = 2 and o_ack bits
 * punctured into them; o_ack > 2: rate-matched HARQ-ACK. b_* are beta-offset table indices, alpha the alpha index. */
typedef struct {
  uint8_t o_ack, b_ack;
  uint8_t o_csi1, b_csi1;
  uint8_t o_csi2, b_csi2;
  uint8_t alpha; /* 0: 0.5, 1: 0.65, 2: 0.8, 3: 1.0 */
  /* CSI part 2 placement. 0: TS 38.212 6.2.7 step 5 as written (phi_l(j * d) over the REs left free by CSI part 1, d from
   * their count). 1: d from the symbol's full RE count, raw RE indices, CSI-part-1 REs stepped over one at a time -- what the
   * OAI UE does (nr_ulsch_ue.c map_uci_common). A UE implementing either is decoded: the CRC decides, learned per RNTI. */
  uint8_t csi2_map;
} nr_uci_hyp_t;

/* REs per layer of each part for a hypothesis on a geometry. */
typedef struct {
  uint32_t q_ack;  /* HARQ-ACK REs: rate-matched (o_ack > 2) or punctured (o_ack <= 2) */
  uint32_t q_rvd;  /* reserved REs (o_ack <= 2 with UCI present) */
  uint32_t q_csi1, q_csi2;
  uint32_t u_re;   /* REs taken from the UL-SCH (rate-matched parts) */
} nr_uci_counts_t;

/* TS 38.212 6.3.2.4.1 counts. false = no UCI or no room for the UL-SCH. */
bool nr_uci_counts(const nr_uci_geom_t *g, const nr_uci_hyp_t *h, nr_uci_counts_t *c);
/* Total UL-SCH REs of the allocation without UCI (G / (N_L * Q_m)). */
uint32_t nr_uci_data_re(const nr_uci_geom_t *g);

/* TS 38.212 6.2.7 inverse: from the codeword's descrambled LLRs (G = data_re * N_L * Q_m) to the UL-SCH LLR stream:
 * UCI REs removed, HARQ-ACK-punctured REs zeroed (erasures). Writes the stream window [j0, j0 + n) (n = 0: all) into out
 * and returns G_ulsch (the full stream length), 0 on an infeasible hypothesis. */
uint32_t nr_uci_demux(const nr_uci_geom_t *g, const nr_uci_hyp_t *h, const int16_t *llr, uint32_t j0, uint32_t n, int16_t *out);

/* One search. tb() decodes the UL-SCH LLR stream of G_ulsch bits (setting the UL-SCH RE count G_ulsch / (N_L Q_m) as
 * the decoder's G) and returns the TB CRC verdict. */
typedef bool (*nr_uci_tb_fn)(void *opaque, const int16_t *ulsch_llr, uint32_t g_ulsch);

typedef struct {
  uint16_t rnti;
  nr_uci_geom_t geom;
  uint32_t tbs_bits;      /* A */
  uint8_t bg, rv;
  int dai;                /* DCI 0_1 UL DAI field value (prior for O_ACK of a dynamic codebook), -1 unknown */
  const int16_t *llr;     /* codeword LLRs, descrambled, G of them */
  uint32_t G;
  int16_t *work;          /* >= G entries: receives the winning UL-SCH stream */
  nr_uci_tb_fn tb;
  void *opaque;
  bool explore;           /* the caller's rate limiter allows the exploration step */
  uint32_t budget_us;     /* CPU budget of the exploration step */
} nr_uci_search_in_t;

typedef struct {
  bool ok;                /* the TB decoded at h; work holds its UL-SCH stream, g_ulsch bits */
  bool learned;           /* ok came from a hypothesis learned earlier for this RNTI */
  nr_uci_hyp_t h;
  nr_uci_counts_t c;
  uint32_t g_ulsch;
  unsigned cb_tests, tb_tests;
  int u_lo, u_hi;         /* the U interval the last code block admitted (-1: not scanned) */
  bool no_rm_uci;         /* the last code block decodes with no UCI RE removed: no rate-matched UCI in this grant */
} nr_uci_search_out_t;

void nr_pusch_uci_blind_search(const nr_uci_search_in_t *in, nr_uci_search_out_t *out);

/* Learned hypotheses of an RNTI (tests and the summary line). */
int nr_pusch_uci_blind_learned(uint16_t rnti, nr_uci_hyp_t *h, uint32_t *hits, int max);
void nr_pusch_uci_blind_forget(uint16_t rnti);
/* Census for the periodic summary: searches, explorations, learned wins, exploration wins, single-block tests, TB tests. */
void nr_pusch_uci_blind_stats(uint64_t out[6]);

#ifdef __cplusplus
}
#endif
#endif
