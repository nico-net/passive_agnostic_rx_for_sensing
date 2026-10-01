/* SPDX-License-Identifier: LicenseRef-CSSL-1.0 */
/*
 * nr_passive_obs.h -- per-grant observation records (PROJECT_MEMORY §21), JSON Lines, schema 1.
 * THIS COMMENT IS THE SINGLE SOURCE OF THE SCHEMA. PROJECT_MEMORY §21 only points here.
 *
 * One JSON object per line, one line per PDSCH/PUSCH grant whose transport block was decoded
 * (TB CRC pass or fail). Written by nr-uesoftmodem when ISAC_OBS_PATH is set (file opened in append
 * mode). Keys are emitted in the order below. Every v1 record is a scheduled-DATA grant: dir=DL is
 * PDSCH data, dir=UL is PUSCH data.
 *
 * Unknown values: EVERY unknown is JSON null. In C a signed integer field holds -1 and a float field
 * holds NAN (non-finite floats are also emitted as null). No integer field has a valid negative value.
 * Unsigned fields (dir, rnti, dmrs_symb_pos, t_mono_ns) are always known.
 *
 * Versioning: adding a key keeps schema 1 (consumers MUST ignore unknown keys). Changing a key's
 * meaning/unit, removing a key, or emitting records that are not scheduled-data grants (SSB, CSI-RS,
 * PDCCH ...) requires NR_PASSIVE_OBS_SCHEMA 2.
 *
 * Coverage (v1): DL = deferred PDSCH queue consumer only (nr_pdsch_passive_queue.c), layout probes
 * excluded; the in-line DL decode used when the queue is not running is NOT recorded. UL = every
 * nr_pusch_passive_decode() call (both callers) with status OK / CRC_FAIL / ZERO_TB; UNSUPPORTED,
 * ERROR and cfr_only calls are not recorded.
 *
 * key                 C type    unit / meaning                                         unknown  DL source                          UL source
 * schema              (const)   NR_PASSIVE_OBS_SCHEMA                                  never    -                                  -
 * abs_slot            int64     receiver monotonic slot of the grant's samples (time   null     job.absolute_slot                  abs_slot arg (0 -> null)
 *                               axis for sensing)
 * t_mono_ns           uint64    CLOCK_MONOTONIC ns when the record was built = decode  never    clock_gettime                      clock_gettime
 *                               COMPLETION (includes queue latency; not air time)
 * frame               int16     SFN 0..1023 of the grant's slot                        null     job.frame_rx                       frame arg (PUSCH slot)
 * slot                int16     slot in frame of the grant (UL: DCI slot + k2)         null     job.nr_slot_rx                     slot arg
 * pci                 int16     physical cell id 0..1007                               null     frame_parms.Nid_cell               frame_parms.Nid_cell
 * dir                 uint8     "DL" | "UL" (JSON string)                              never    NR_OBS_DIR_DL                      NR_OBS_DIR_UL
 * rnti                uint16    CRC-recovered RNTI (decimal)                           never    job.rnti                           g->rnti
 * rnti_class          int8      nr_blind_rnti_class_t: 0 C,1 TC,2 SI,3 RA,4 P          null     job.rnti_class                     -1 (no class on UL)
 * start_rb            int16     lowest allocated PRB, CRB-indexed (BWP start + offset) null     BWPStart+freq_alloc.first_rb       g->bwp_start+g->start_rb
 * nb_rb               int16     allocated PRB COUNT (allocation may be non-contiguous) null     freq_alloc.num_rbs                 g->num_rb
 * start_sym           int8      first OFDM symbol S                                    null     dlsch_pdu.start_symbol             g->start_symbol
 * nb_sym              int8      symbol count L                                         null     dlsch_pdu.number_symbols           g->num_symbols
 * mcs                 int8      MCS index                                              null     job.grant.mcs                      g->mcs
 * mcs_table           int8      0 qam64, 1 qam256, 2 qam64LowSE                        null     job.grant.mcs_table                g->mcs_table
 * qm                  int8      modulation order (bits/symbol)                         null     dec.cw.qamModOrder                 out->qam_mod_order
 * nl                  int8      layers (rank)                                          null     dec.cw.Nl (DM-RS port count)       g->nrOfLayers
 * dmrs_symb_pos       uint16    DM-RS symbol bitmap, bit l = symbol l (decimal)        never    dlsch_pdu.dlDmrsSymbPos            g->ul_dmrs_symb_pos
 * dmrs_scrambling_id  int32     DM-RS scrambling identity 0..65535                     null     dlsch_pdu.dlDmrsScramblingId       g->ul_dmrs_scrambling_id
 * tbs                 int32     transport block size, BITS                             null     dec.cw.TBS                         out->tbs_bytes*8
 * harq_pid            int8      HARQ process                                           null(*)  job.grant.harq_pid                 g->harq_pid
 * rv                  int8      redundancy version as signalled                        null     job.grant.rv                       g->rv
 * ndi                 int8      new-data indicator as signalled                        null(*)  job.grant.ndi                      g->ndi
 * crc                 int8      1 TB CRC pass, 0 fail (nr_obs_crc_t)                   null(**) decode status                      out->status
 * nvar                float     DL noise variance, linear, receiver-internal int16^2   null     dec.nvar                           NAN
 *                               scale: compare only within one run / gain / config
 * snr_db              float     UL post-estimation SNR, dB, receiver-internal          null     NAN                                out->snr_db
 * fo_comp_hz          float     FO the receiver digitally removed from these samples   null     job.fo_hz                          fo_hz arg
 *                               before the FFT, Hz; + = received carrier above LO.
 *                               NOT a per-grant measurement; 0 = no digital comp.
 * delay_samples       float     UL DM-RS CIR peak offset vs the FFT window, samples at null     NAN                                out->est_delay
 *                               fs_hz; + = later. 0 also means "no clear peak"
 * carrier_hz          int64     carrier centre frequency of this direction, Hz         null     frame_parms.dl_CarrierFreq         frame_parms.ul_CarrierFreq
 * scs_khz             int16     subcarrier spacing, kHz (grant BW = nb_rb*12*scs_khz)  null     frame_parms.subcarrier_spacing/1e3 same
 * fs_hz               int64     receiver sample rate = N_fft * SCS, Hz                 null     frame_parms.samples_per_subframe*1e3 same
 *
 * (*)  null for DL grants with rnti_class SI/RA/P (DCI 1_0: field reserved/absent, TS 38.212 7.3.1.2.1).
 * (**) null for UL ZERO_TB: all-zero TB, the CRC passes by construction, so it is not a verified decode.
 * [KNOWN ISSUE] UL snr_db holds the CFR mean power in dB (not an SNR) when the noise estimate is 0
 *              (nr_pusch_passive_decode.c:1120 vs :1381).
 * Evidence: schema [IMPLEMENTED, NOT VALIDATED] until A3 Step 9 (rfsim) passes ([SIM VERIFIED]); no OTA.
 */
#ifndef NR_PASSIVE_OBS_H
#define NR_PASSIVE_OBS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define NR_PASSIVE_OBS_SCHEMA 1
typedef enum { NR_OBS_DIR_DL = 0, NR_OBS_DIR_UL = 1 } nr_obs_dir_t;
typedef enum { NR_OBS_CRC_NA = -1, NR_OBS_CRC_FAIL = 0, NR_OBS_CRC_OK = 1 } nr_obs_crc_t;
/* One observed grant; JSON key = field name except `dir` (string). See the table above. */
typedef struct {
  int64_t abs_slot;
  uint64_t t_mono_ns;
  int16_t frame, slot;
  int16_t pci;
  uint8_t dir;                   // nr_obs_dir_t
  uint16_t rnti;
  int8_t rnti_class;             // nr_blind_rnti_class_t, -1 unknown
  int16_t start_rb, nb_rb;       // CRB-indexed lowest PRB; PRB count
  int8_t start_sym, nb_sym;
  int8_t mcs, mcs_table, qm, nl; // nl = layers (rank)
  uint16_t dmrs_symb_pos;        // bitmap
  int32_t dmrs_scrambling_id;    // -1 unknown
  int32_t tbs;                   // BITS, -1 unknown
  int8_t harq_pid, rv, ndi;      // -1 unknown/absent
  int8_t crc;                    // nr_obs_crc_t
  float nvar;                    // DL only, NAN on UL
  float snr_db;                  // UL only, NAN on DL
  float fo_comp_hz;              // applied FO compensation, NAN unknown
  float delay_samples;           // UL only, NAN on DL
  int64_t carrier_hz;            // -1 unknown
  int16_t scs_khz;               // -1 unknown
  int64_t fs_hz;                 // -1 unknown
} nr_passive_obs_t;
/* Serialises one record as a single JSON object, no newline, NUL-terminated.
 * Returns the byte count (excluding NUL), or -1 if it does not fit in n. 1024 bytes always suffice. */
int nr_passive_obs_to_json(const nr_passive_obs_t *o, char *buf, size_t n);
/* Lifecycle. open/close are called from one controlling thread (any thread), never concurrently with
 * each other; push and stats may be called from any thread at any time, including before open and
 * after close.
 * open: appends to `path`, starts the writer thread, resets the counters. capacity = ring slots
 *       (whole records, any value >= 1). Returns false (and changes nothing) if already open,
 *       capacity == 0, or the file/ring cannot be created. Re-open after close is allowed. */
bool nr_passive_obs_open(const char *path, uint32_t capacity);
/* close: stops accepting pushes, writes every record already accepted, flushes, joins the writer,
 *        closes the file. A no-op when not open. */
void nr_passive_obs_close(void);
/* push: never blocks on I/O (a short mutex only). Copies *o into the ring. Returns true if accepted.
 *       When not open: returns false and counts nothing. When the ring is full: returns false and
 *       increments `dropped`. MT-safe, also against a concurrent close. */
bool nr_passive_obs_push(const nr_passive_obs_t *o);
/* stats: pushed = accepted, written = lines fully written to the file, dropped = ring-full rejects,
 *        since the last successful open (still readable after close). After close,
 *        pushed - written = records lost to I/O errors. Any pointer may be NULL. MT-safe. */
void nr_passive_obs_stats(uint64_t *pushed, uint64_t *written, uint64_t *dropped);
#ifdef __cplusplus
}
#endif
#endif
