/*
 * Harvest the DEDICATED radio configuration from an overheard, UNCIPHERED RRCSetup (Msg4).
 *
 * WHY THIS EXISTS. DCI 1_0 and 0_0 are FALLBACK formats: their field widths derive entirely from
 * broadcast information (CORESET#0's N_RB, the initial DL/UL BWP), which is why this receiver
 * already decodes them with no search at all. DCI 1_1 and 0_1 are not like that. Their widths come
 * from pdsch-Config / pusch-Config / ServingCellConfig / physicalCellGroupConfig -- bwp_indicator,
 * TDRA table size, antenna ports, rate matching, ZP-CSI-RS, DAI, SRS request, CBG -- and NONE of
 * that is ever broadcast. The blind sweep exists purely as a brute-force substitute for information
 * that is not on the air.
 *
 * There is exactly one moment when it IS on the air in the clear. AS security is activated by
 * SecurityModeCommand, which follows RRCSetupComplete, so Msg4/RRCSetup travels on SRB0 with no
 * ciphering and no integrity protection. RRCReconfiguration, which carries the same IEs later, is
 * sent after security and is unreadable. So this is the only window, and it is per-UE: it opens
 * every time any UE attaches to the cell.
 *
 * MEASURED PREREQUISITE (2026-09-21, Swisscom n78): this cell's SIB1 carries NO
 * commonControlResourceSet, so the dedicated CORESET cannot be obtained from broadcast at all --
 * confirmed, not assumed. That is what makes this path the only alternative to sweeping.
 *
 * NO RA-RNTI/TC-RNTI GATE, DELIBERATELY. The obvious design is to first verify an RA-RNTI, harvest
 * the TC-RNTI from its RAR, and only then look for Msg4 addressed to that TC-RNTI. That chain works
 * but needs three things to land in the slots we happen to process. It is unnecessary: a successful
 * uper_decode of a multi-hundred-bit RRCSetup is self-validating -- the probability that a random
 * transport block decodes as a well-formed DL-CCCH RRCSetup AND yields a plausible CellGroupConfig
 * is negligible. So this runs on ANY CRC-verified transport block and lets the ASN.1 decoder be the
 * evidence, which strictly dominates gating on an RNTI we might never verify.
 *
 * SCOPE: read-only. Nothing here touches this receiver's own RRC/MAC state -- the config belongs to
 * a foreign UE and is published only as a prior for the blind search.
 */

#include "PHY/NR_UE_TRANSPORT/nr_pdcch_sib1_prior.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>

#include "common/utils/LOG/log.h"
#include "NR_DL-CCCH-Message.h"
#include "NR_CellGroupConfig.h"
#include "NR_ControlResourceSet.h"
#include "NR_SearchSpace.h"

/* TS 38.321 6.2.1: DL-SCH subheader is R(1) F(1) LCID(6); CCCH is LCID 0. Fixed-size CEs carry no
 * length field, so the walk must know their sizes to step over them -- mirrors the walker in
 * nr_passive_mac_ta.c rather than inventing a second convention. */
#define DLSCH_LCID_CCCH      0
#define DLSCH_LCID_PADDING   63
#define DLSCH_LCID_TA_CMD    61
#define DLSCH_LCID_CON_RES   62

static int dlsch_fixed_ce_len(uint8_t lcid)
{
  switch (lcid) {
    case DLSCH_LCID_TA_CMD:  return 1;
    case DLSCH_LCID_CON_RES: return 6;
    default:                 return -1;
  }
}

/** Locate the CCCH SDU (LCID 0) inside a DL-SCH MAC PDU. Returns NULL when there is none. */
static const uint8_t *dlsch_find_ccch(const uint8_t *pdu, uint32_t len, uint32_t *sdu_len)
{
  uint32_t i = 0;
  while (i < len) {
    const uint8_t sub = pdu[i];
    const uint8_t lcid = sub & 0x3F;
    const bool F = (sub & 0x40) != 0;
    i += 1;
    if (lcid == DLSCH_LCID_PADDING) {
      return NULL; /* padding runs to the end */
    }
    const int fixed = dlsch_fixed_ce_len(lcid);
    if (fixed >= 0) {
      i += (uint32_t)fixed;
      continue;
    }
    if (lcid > 32) {
      return NULL; /* unknown CE of unknown size: stop rather than walk into noise */
    }
    uint32_t l;
    if (F) {
      if (i + 2 > len) return NULL;
      l = ((uint32_t)pdu[i] << 8) | pdu[i + 1];
      i += 2;
    } else {
      if (i + 1 > len) return NULL;
      l = pdu[i];
      i += 1;
    }
    if (i + l > len) return NULL;
    if (lcid == DLSCH_LCID_CCCH) {
      *sdu_len = l;
      return pdu + i;
    }
    i += l;
  }
  return NULL;
}

/* Report what a decoded dedicated config actually contains. This is the measurement the whole path
 * exists to make: if this gNB defers pdcch-Config/pusch-Config to RRCReconfiguration (post-security,
 * ciphered) then 0_1 and 1_1 are PERMANENTLY sweep-only on this cell, and that is a hard limit worth
 * establishing from one decode rather than inferring from months of non-convergence. */
static void report_dedicated(const NR_CellGroupConfig_t *cg)
{
  const NR_ServingCellConfig_t *sc =
      (cg->spCellConfig != NULL) ? cg->spCellConfig->spCellConfigDedicated : NULL;
  if (sc == NULL) {
    LOG_A(PHY, "SENSING: RRCSETUP HARVEST: CellGroupConfig decoded but carries NO "
               "spCellConfigDedicated -- this gNB defers the dedicated config to "
               "RRCReconfiguration (ciphered). DCI 0_1/1_1 remain sweep-only on this cell.\n");
    return;
  }

  int n_coreset = 0, n_ss = 0, n_dl_bwp = 0, n_tda = 0;
  bool have_pdcch = false, have_pdsch = false;

  if (sc->downlinkBWP_ToAddModList != NULL) {
    n_dl_bwp = sc->downlinkBWP_ToAddModList->list.count;
  }
  const NR_BWP_DownlinkDedicated_t *dl_ded = sc->initialDownlinkBWP;
  if (dl_ded != NULL) {
    if (dl_ded->pdcch_Config != NULL
        && dl_ded->pdcch_Config->present == NR_SetupRelease_PDCCH_Config_PR_setup) {
      const NR_PDCCH_Config_t *pc = dl_ded->pdcch_Config->choice.setup;
      have_pdcch = true;
      if (pc->controlResourceSetToAddModList) n_coreset = pc->controlResourceSetToAddModList->list.count;
      if (pc->searchSpacesToAddModList)       n_ss      = pc->searchSpacesToAddModList->list.count;
    }
    if (dl_ded->pdsch_Config != NULL
        && dl_ded->pdsch_Config->present == NR_SetupRelease_PDSCH_Config_PR_setup) {
      const NR_PDSCH_Config_t *ps = dl_ded->pdsch_Config->choice.setup;
      have_pdsch = true;
      if (ps->pdsch_TimeDomainAllocationList != NULL
          && ps->pdsch_TimeDomainAllocationList->present
                 == NR_SetupRelease_PDSCH_TimeDomainResourceAllocationList_PR_setup) {
        n_tda = ps->pdsch_TimeDomainAllocationList->choice.setup->list.count;
      }
    }
  }

  /* n_dl_bwp and n_tda are the two field widths that produced the long-unexplained 3-bit gap in
   * this project's DCI 1_1 size formula (bwp_indicator and the TDA field). Printing them is the
   * point: they turn a swept length into a computed one. */
  LOG_A(PHY,
        "SENSING: RRCSETUP HARVEST: dedicated config PRESENT -- pdcch_Config=%d (coresets=%d "
        "searchspaces=%d) pdsch_Config=%d dl_bwps=%d pdsch_tda_entries=%d  => bwp_indicator and "
        "TDA widths are now DERIVABLE, so DCI 1_1 length need not be swept\n",
        have_pdcch, n_coreset, n_ss, have_pdsch, n_dl_bwp, n_tda);

  /* Publish the first dedicated CORESET as the search prior. Conversions match
   * nr_ue_dci_configuration.c's canonical decoding so the value is directly usable. */
  if (have_pdcch && n_coreset > 0) {
    const NR_ControlResourceSet_t *cs =
        dl_ded->pdcch_Config->choice.setup->controlResourceSetToAddModList->list.array[0];
    nr_pdcch_sib1_prior_t pr;
    const nr_pdcch_sib1_prior_t *cur = nr_pdcch_sib1_prior_get();
    if (cur != NULL) {
      pr = *cur; /* keep the SIB1-derived fields; only the CORESET is being replaced */
    } else {
      memset(&pr, 0, sizeof(pr));
    }
    pr.coreset_valid = true;
    pr.coreset_id = (uint8_t)cs->controlResourceSetId;
    pr.duration = (uint8_t)cs->duration;
    for (int i = 0; i < 6 && i < cs->frequencyDomainResources.size; i++) {
      pr.frequency_domain_resource[i] = cs->frequencyDomainResources.buf[i];
    }
    if (cs->cce_REG_MappingType.present == NR_ControlResourceSet__cce_REG_MappingType_PR_interleaved) {
      const struct NR_ControlResourceSet__cce_REG_MappingType__interleaved *il =
          cs->cce_REG_MappingType.choice.interleaved;
      pr.interleaved = true;
      pr.reg_bundle_size =
          (uint8_t)((il->reg_BundleSize == NR_ControlResourceSet__cce_REG_MappingType__interleaved__reg_BundleSize_n6)
                        ? 6 : (2 + il->reg_BundleSize));
      pr.interleaver_size =
          (uint8_t)((il->interleaverSize == NR_ControlResourceSet__cce_REG_MappingType__interleaved__interleaverSize_n6)
                        ? 6 : (2 + il->interleaverSize));
      pr.shift_index = (uint16_t)(il->shiftIndex != NULL ? *il->shiftIndex : 0);
    } else {
      pr.interleaved = false;
      pr.reg_bundle_size = 0;
      pr.interleaver_size = 0;
      pr.shift_index = 0;
    }
    pr.pdcch_dmrs_scrambling_id =
        (uint16_t)(cs->pdcch_DMRS_ScramblingID != NULL ? *cs->pdcch_DMRS_ScramblingID : 0);
    nr_pdcch_sib1_prior_set(&pr);
    LOG_A(PHY,
          "SENSING: RRCSETUP HARVEST: DEDICATED CORESET id=%u dur=%u %s bundle=%u interleaver=%u "
          "shift=%u dmrs_id=%u -- the 36,000-hypothesis extent x mapping sweep is now unnecessary\n",
          pr.coreset_id, pr.duration, pr.interleaved ? "interleaved" : "non-interleaved",
          pr.reg_bundle_size, pr.interleaver_size, pr.shift_index, pr.pdcch_dmrs_scrambling_id);
  }
}

void nr_passive_rrc_harvest(const uint8_t *tb, uint32_t tb_bytes);
void nr_passive_rrc_harvest(const uint8_t *tb, uint32_t tb_bytes)
{
  if (tb == NULL || tb_bytes < 4) {
    return;
  }
  /* CENSUS. Without this a null result is uninterpretable: "no RRCSetup on this cell" and "this
   * code never ran" produce identical (silent) logs. Counts every stage so the outcome can be read
   * either way. */
  static _Atomic unsigned long s_tb = 0, s_ccch = 0, s_ccch_ok = 0, s_setup = 0;
  const unsigned long n_tb = atomic_fetch_add_explicit(&s_tb, 1, memory_order_relaxed) + 1;

  uint32_t sdu_len = 0;
  const uint8_t *ccch = dlsch_find_ccch(tb, tb_bytes, &sdu_len);
  if (ccch != NULL && sdu_len >= 2) {
    atomic_fetch_add_explicit(&s_ccch, 1, memory_order_relaxed);
  }
  if ((n_tb % 200) == 0) {
    LOG_A(PHY,
          "SENSING: RRCHARVEST census: tb_examined=%lu ccch_sdus=%lu dl_ccch_decoded=%lu rrcsetup=%lu\n",
          n_tb,
          atomic_load_explicit(&s_ccch, memory_order_relaxed),
          atomic_load_explicit(&s_ccch_ok, memory_order_relaxed),
          atomic_load_explicit(&s_setup, memory_order_relaxed));
  }
  if (ccch == NULL || sdu_len < 2) {
    return; /* no CCCH in this PDU -- the overwhelmingly common case */
  }

  NR_DL_CCCH_Message_t *msg = NULL;
  const asn_dec_rval_t rv = uper_decode(NULL, &asn_DEF_NR_DL_CCCH_Message, (void **)&msg,
                                        (uint8_t *)ccch, sdu_len, 0, 0);
  if (rv.code != RC_OK || msg == NULL) {
    ASN_STRUCT_FREE(asn_DEF_NR_DL_CCCH_Message, msg);
    return; /* not a DL-CCCH message: this IS the validation, so failure is expected and silent */
  }

  atomic_fetch_add_explicit(&s_ccch_ok, 1, memory_order_relaxed);
  if (msg->message.present == NR_DL_CCCH_MessageType_PR_c1
      && msg->message.choice.c1 != NULL
      && msg->message.choice.c1->present == NR_DL_CCCH_MessageType__c1_PR_rrcSetup) {
    const NR_RRCSetup_t *setup = msg->message.choice.c1->choice.rrcSetup;
    if (setup != NULL && setup->criticalExtensions.present == NR_RRCSetup__criticalExtensions_PR_rrcSetup) {
      const NR_RRCSetup_IEs_t *ies = setup->criticalExtensions.choice.rrcSetup;
      atomic_fetch_add_explicit(&s_setup, 1, memory_order_relaxed);
      LOG_A(PHY, "SENSING: RRCSETUP HARVEST: overheard an UNCIPHERED RRCSetup (%u byte CCCH SDU)\n",
            sdu_len);
      NR_CellGroupConfig_t *cg = NULL;
      const asn_dec_rval_t rv2 = uper_decode(NULL, &asn_DEF_NR_CellGroupConfig, (void **)&cg,
                                             ies->masterCellGroup.buf, ies->masterCellGroup.size, 0, 0);
      if (rv2.code == RC_OK && cg != NULL) {
        report_dedicated(cg);
      } else {
        LOG_W(PHY, "SENSING: RRCSETUP HARVEST: masterCellGroup did not decode (%zu bytes)\n",
              (size_t)ies->masterCellGroup.size);
      }
      ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
    }
  }
  ASN_STRUCT_FREE(asn_DEF_NR_DL_CCCH_Message, msg);
}
