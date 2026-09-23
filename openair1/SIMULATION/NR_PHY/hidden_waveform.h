/* Transmitter-only fixture. Never linked into the passive receiver. */
#include "PHY/NR_TRANSPORT/nr_dci.h"

static void hidden_truth_dci(FILE *truth, int frame, int slot, const char *format,
                             const nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p)
{
  for (int i = 0; i < p->numDlDci; ++i) {
    const nfapi_nr_dl_dci_pdu_t *d = &p->dci_pdu[i];
    fprintf(truth, "{\"kind\":\"dci\",\"frame\":%d,\"slot\":%d,\"format\":\"%s\","
            "\"rnti\":%u,\"scrambling_rnti\":%u,\"nid\":%u,\"bits\":%u,\"al\":%u,\"cce\":%u,"
            "\"bwp_start\":%u,\"duration\":%u,\"start_symbol\":%u,\"payload_le\":\"",
            frame, slot, format, d->RNTI, d->ScramblingRNTI, d->ScramblingId,
            d->PayloadSizeBits, d->AggregationLevel, d->CceIndex,
            p->BWPStart, p->DurationSymbols, p->StartSymbolIndex);
    for (int b = 0; b < (d->PayloadSizeBits + 7) / 8; ++b)
      fprintf(truth, "%02x", d->Payload[b]);
    fprintf(truth, "\",\"frequency_resources\":\"");
    for (int b = 0; b < 6; ++b) fprintf(truth, "%02x", p->FreqDomainResource[b]);
    fprintf(truth, "\"}\n");
  }
}

static void hidden_coreset_bitmap(nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p, int first_group, int groups,
                                  uint16_t cce_index)
{
  memset(p->FreqDomainResource, 0, sizeof(p->FreqDomainResource));
  for (int g = first_group; g < first_group + groups; ++g)
    p->FreqDomainResource[g / 8] |= (uint8_t)(0x80 >> (g % 8));
  p->BWPStart = 0;
  p->BWPSize = 106;
  p->DurationSymbols = 1;
  p->StartSymbolIndex = 0;
  for (int i = 0; i < p->numDlDci; ++i) {
    AssertFatal(cce_index + p->dci_pdu[i].AggregationLevel <= groups,
                "Hidden CCE outside CORESET\n");
    p->dci_pdu[i].CceIndex = cce_index;
  }
}

static void hidden_add_second_dl_coreset(nfapi_nr_dl_tti_request_body_t *body, uint16_t second_rnti,
                                         uint16_t cce_index)
{
  int src = -1;
  for (int i = 0; i < body->nPDUs; ++i)
    if (body->dl_tti_pdu_list[i].PDUType == NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE) {
      nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p = &body->dl_tti_pdu_list[i].pdcch_pdu.pdcch_pdu_rel15;
      if (p->numDlDci > 0 && p->dci_pdu[0].RNTI != 65535) src = i;
    }
  AssertFatal(src >= 0 && body->nPDUs < NFAPI_NR_MAX_DL_TTI_PDUS, "No room for second native PDCCH PDU\n");
  hidden_coreset_bitmap(&body->dl_tti_pdu_list[src].pdcch_pdu.pdcch_pdu_rel15, 0, 8, cce_index);
  const int dst = body->nPDUs++;
  body->dl_tti_pdu_list[dst] = body->dl_tti_pdu_list[src];
  nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p2 = &body->dl_tti_pdu_list[dst].pdcch_pdu.pdcch_pdu_rel15;
  hidden_coreset_bitmap(p2, 8, 8, cce_index);
  /* A genuinely distinct second CORESET: interleaved CCE-to-REG mapping with the standard
   * cell-ID shift. A wide non-interleaved scan cannot decode it, so the receiver must retain the
   * first configuration and discover/manage this one independently. */
  p2->CceRegMappingType = NFAPI_NR_CCE_REG_MAPPING_INTERLEAVED;
  p2->RegBundleSize = 6;
  p2->InterleaverSize = 2;
  p2->ShiftIndex = p2->dci_pdu[0].ScramblingId;
  for (int i = 0; i < p2->numDlDci; ++i)
    p2->dci_pdu[i].RNTI = second_rnti;
}

static int hidden_waveform(PHY_VARS_gNB *tx, gNB_MAC_INST *mac, NR_UE_info_t *ue,
                           double fs, int frames, const char *directory)
{
  AssertFatal(ue->current_DL_BWP.dci_format == NR_DL_DCI_FORMAT_1_1 &&
              ue->current_UL_BWP.dci_format == NR_UL_DCI_FORMAT_0_1, "Fixture needs dedicated formats\n");
  const bool multi_coreset = getenv("ISAC_HIDDEN_MULTI_CORESET") != NULL;
  const uint16_t second_rnti = (uint16_t)(ue->rnti ^ 0x3101u);
  char path[1024];
  snprintf(path, sizeof(path), "%s/tx.sc16", directory);
  FILE *iq = fopen(path, "wb");
  snprintf(path, sizeof(path), "%s/truth.jsonl", directory);
  FILE *truth = fopen(path, "w");
  AssertFatal(iq && truth, "Cannot open waveform output\n");
  NR_DL_FRAME_PARMS *fp = &tx->frame_parms;
  AssertFatal(fp->nb_antennas_tx == 1 && fp->numerology_index == 1, "Initial fixture supports one TX and mu=1\n");
  const plmn_id_t plmn = {.mcc = 1, .mnc = 1, .mnc_digit_length = 2};
  get_softmodem_params()->phy_test = 0;
  nr_mac_configure_sib1(mac, &plmn, 0x1579, 1);
  get_softmodem_params()->phy_test = 1;
  initNamedTpool("n", &tx->threadPool, true, "waveform-tx");
  initNotifiedFIFO(&tx->L1_tx_out);
  NR_Sched_Rsp_t *rsp = calloc(1, sizeof(*rsp));
  c16_t *time = calloc(fp->samples_per_frame, sizeof(*time));
  c16_t *frequency = calloc(fp->ofdm_symbol_size * 14, sizeof(*frequency));
  AssertFatal(rsp && time && frequency, "Allocation failed\n");
  fprintf(truth, "{\"kind\":\"cell\",\"pci\":%d,\"rnti\":%u,\"rb\":%u,\"fs\":%.0f,"
          "\"center_hz\":%llu,\"frames\":%d,\"dl_format\":%d,\"ul_format\":%d}\n",
          *mac->common_channels[0].ServingCellConfigCommon->physCellId, ue->rnti, fp->N_RB_DL, fs,
          (unsigned long long)fp->dl_CarrierFreq, frames, ue->current_DL_BWP.dci_format, ue->current_UL_BWP.dci_format);
  for (int f = 0; f < frames; ++f) {
    memset(time, 0, fp->samples_per_frame * sizeof(*time));
    for (int s = 0; s < fp->slots_per_frame; ++s) {
      reset_sched_response(rsp, f % 1024, s, 0, 0);
      NR_SCHED_LOCK(&mac->sched_lock);
      memset(mac->common_channels[0].vrb_map[0], 0, MAX_BWP_SIZE * sizeof(uint16_t));
      clear_nr_nfapi_information(mac, 0, f % 1024, s);
      /* Enable SA broadcast scheduling only; no receiver exists in this process. */
      get_softmodem_params()->phy_test = 0;
      schedule_nr_mib(0, f % 1024, s, &rsp->DL_req);
      schedule_nr_sib1(0, f % 1024, s, &rsp->DL_req, &rsp->TX_req);
      get_softmodem_params()->phy_test = 1;
      /* Native DL scheduler produces both the DCI and its PDSCH. */
      if (f >= 8 && (f & 1) && (s == 3 || s == 4) && is_dl_slot(s, &mac->frame_structure)) {
        ue->UE_sched_ctrl.harq_processes[s].ndi = (f / 2) & 1;
        g_rbStart = f % 13;
        g_rbSize = 24 + f % 37;
        ue->UE_sched_ctrl.harq_processes[s].round = 0;
        nr_schedule_ue_spec(0, f % 1024, s, &rsp->DL_req, &rsp->TX_req);
        if (multi_coreset)
          hidden_add_second_dl_coreset(&rsp->DL_req.dl_tti_request_body, second_rnti,
                                       (f & 2) ? 4 : 0);
      }
      /* UL grants occupy the downlink. Payload packing and coding are native OAI. */
      if (f >= 8 && (f & 1) && s == 2 && is_dl_slot(s, &mac->frame_structure)) {
        nfapi_nr_ul_dci_request_t *request = &rsp->UL_dci_req;
        request->SFN = f % 1024;
        request->Slot = s;
        request->numPdus = 1;
        memset(&request->ul_dci_pdu_list[0], 0, sizeof(request->ul_dci_pdu_list[0]));
        request->ul_dci_pdu_list[0].PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
        nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p = &request->ul_dci_pdu_list[0].pdcch_pdu.pdcch_pdu_rel15;
        NR_UE_sched_ctrl_t *sc = &ue->UE_sched_ctrl;
        nr_configure_pdcch(p, sc->coreset, &sc->sched_pdcch);
        NR_tda_info_t tda = get_ul_tda_info(&ue->current_UL_BWP, sc->coreset->controlResourceSetId,
                                            sc->search_space->searchSpaceType->present, TYPE_C_RNTI_, 0);
        AssertFatal(tda.valid_tda && is_ul_slot((s + tda.k2) % fp->slots_per_frame, &mac->frame_structure),
                    "UL grant does not schedule an uplink slot\n");
        int nc = 0;
        int al = 4;
        find_aggregation_candidates(&al, &nc, sc->search_space, 4);
        int cce = find_pdcch_candidate(mac, 0, al, nc, 0, &sc->sched_pdcch, sc->coreset,
                                      get_Y(sc->search_space, s, ue->rnti));
        AssertFatal(cce >= 0, "No valid UL CCE\n");
        uint16_t ant = 0;
        nfapi_nr_dl_dci_pdu_t *d = prepare_dci_pdu(p, mac->common_channels[0].ServingCellConfigCommon,
                                                sc->search_space, sc->coreset, &ant, al, cce, 0, ue->rnti);
        p->numDlDci++;
        dci_pdu_rel15_t payload = {0};
        payload.frequency_domain_assignment.val = PRBalloc_to_locationandbandwidth0(8, 3 + f % 7, ue->current_UL_BWP.BWPSize);
        payload.time_domain_assignment.val = 0;
        payload.mcs = 5 + f % 5;
        payload.ndi = (f / 2) & 1;
        payload.harq_pid.val = f % 16;
        payload.tpc = 1;
        payload.ulsch_indicator = 1;
        fill_dci_pdu_rel15(&ue->sc_info, &ue->current_DL_BWP, &ue->current_UL_BWP, d, &payload,
                           NR_UL_DCI_FORMAT_0_1, TYPE_C_RNTI_, 0, sc->search_space, sc->coreset,
                           ue->pdsch_HARQ_ACK_Codebook, mac->cset0_bwp_size);
        if (multi_coreset) {
          const uint16_t hidden_cce = (f & 2) ? 4 : 0;
          hidden_coreset_bitmap(p, 0, 8, hidden_cce);
          request->ul_dci_pdu_list[1] = request->ul_dci_pdu_list[0];
          nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p2 =
              &request->ul_dci_pdu_list[1].pdcch_pdu.pdcch_pdu_rel15;
          hidden_coreset_bitmap(p2, 8, 8, hidden_cce);
          p2->CceRegMappingType = NFAPI_NR_CCE_REG_MAPPING_INTERLEAVED;
          p2->RegBundleSize = 6;
          p2->InterleaverSize = 2;
          p2->ShiftIndex = p2->dci_pdu[0].ScramblingId;
          for (int i = 0; i < p2->numDlDci; ++i)
            p2->dci_pdu[i].RNTI = second_rnti;
          request->numPdus = 2;
          hidden_truth_dci(truth, f % 1024, s, "0_1", p2);
        }
        hidden_truth_dci(truth, f % 1024, s, "0_1", p);
      }
      NR_SCHED_UNLOCK(&mac->sched_lock);
      nfapi_nr_dl_tti_request_body_t *body = &rsp->DL_req.dl_tti_request_body;
      for (int i = 0; i < body->nPDUs; ++i)
        if (body->dl_tti_pdu_list[i].PDUType == NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE) {
          const nfapi_nr_dl_tti_pdcch_pdu_rel15_t *p = &body->dl_tti_pdu_list[i].pdcch_pdu.pdcch_pdu_rel15;
          hidden_truth_dci(truth, f % 1024, s, p->dci_pdu[0].RNTI == 65535 ? "SI_1_0" : "1_1", p);
        }
      get_softmodem_params()->phy_test = 0;
      phy_procedures_gNB_TX(tx, &rsp->DL_req, &rsp->TX_req, &rsp->UL_dci_req, f % 1024, s);
      get_softmodem_params()->phy_test = 1;
      fft_shift(tx->common_vars.txdataF[0], fp->ofdm_symbol_size, fp->N_RB_DL,
                frequency, fp->ofdm_symbol_size, 0, 14);
      bool used[14];
      for (int i = 0; i < 14; ++i) used[i] = true;
      nr_normal_prefix_mod(frequency, time + get_samples_slot_timestamp(fp, s), 14, fp, s, used);
    }
    AssertFatal(fwrite(time, sizeof(*time), fp->samples_per_frame, iq) == fp->samples_per_frame, "Short IQ write\n");
  }
  AssertFatal(fclose(iq) == 0 && fclose(truth) == 0, "Output close failed\n");
  fprintf(stderr, "HIDDEN_WAVEFORM_COMPLETE frames=%d samples=%llu\n", frames,
          (unsigned long long)frames * fp->samples_per_frame);
  return 0;
}
