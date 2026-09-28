/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*! \file openair2/LAYER2/NR_MAC_UE/tests/test_nr_ue_mib_blind_handoff.cpp
 * \brief MIB dmrs-TypeA-Position -> blind PDCCH monitor handoff on a cell WITHOUT CORESET#0 (NSA,
 * FR1 kSSB >= 24).
 *
 * The MIB is delivered through the same MAC entry points production uses: the PHY-indication fields
 * handle_bcch_bch() (NR_IF_Module.c) writes before handing the 3-byte PDU to RRC, then
 * nr_rrc_mac_config_req_mib() (what RRC calls with the decoded MIB) and nr_rrc_mac_sched_sib(1) (what
 * RRC calls right after). Nothing in the blind monitor is hand-set to the value under test: the
 * monitor is REAL (nr_pdcch_blind_monitor library, not stubbed) and is only ever read back.
 *
 * Each scenario runs in its own forked child (EXPECT_EXIT) because both the MAC instance table and
 * the blind monitor's config are process globals with no reset; forking keeps every scenario
 * starting from the pristine process state and keeps --gtest_shuffle order-independent.
 */

#include "gtest/gtest.h"
#include "gtest/gtest-spi.h"
extern "C" {
#include "common/platform_types.h"
#include "openair2/LAYER2/NR_MAC_UE/mac_proto.h"
#include "executables/softmodem-common.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"
#include "common/utils/ocp_itti/intertask_interface.h"
#include "openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor.h"
#include "openair1/PHY/NR_UE_TRANSPORT/nr_pdcch_blind_monitor_rt.h"
/* Real signatures from openair1/PHY/NR_REFSIG/nr_refsig.h, declared by hand (as
 * nr_pdcch_blind_monitor_test.cc does): that header's VLA prototypes are not valid C++. */
uint32_t *nr_gold_pdcch(int N_RB_DL, int symbols_per_slot, unsigned short n_idDMRS, int ns, int l);
void nr_pdcch_dmrs_ref(const unsigned int *nr_gold_pdcch, c16_t *output, unsigned short nb_rb_corset);
/* dci_nr.c (linked for the blind monitor's PDCCH helpers) also defines nr_rx_pdcch_symbol(), whose
 * PHY channel-estimation callees this test never reaches. Link-only, as in
 * nr_pdcch_blind_monitor_test.cc. */
void nr_pdcch_channel_estimation(void) {}
void nr_channel_level(void) {}
uint8_t log2_approx(uint32_t x)
{
  (void)x;
  return 0;
}

static softmodem_params_t softmodem_params;

softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}

/* Link-only stubs for the RRC/RLC/ITTI edges of MAC_UE_NR -- same set as test_nr_ue_ra_procedures.
 * None is on the MIB path exercised here. */
void nr_mac_rrc_ra_ind(const module_id_t mod_id, bool success)
{
  UNUSED(mod_id);
  UNUSED(success);
}
void nr_mac_rrc_msg3_ind(const module_id_t mod_id, const int rnti, bool prepare_payload)
{
  UNUSED(mod_id);
  UNUSED(rnti);
  UNUSED(prepare_payload);
}
void nr_mac_rlc_status_ind(uint16_t ue_id, frame_t frame, int n_ch, const logical_chan_id_t *ch, mac_rlc_status_resp_t *ret)
{
  UNUSED(ue_id);
  UNUSED(frame);
  UNUSED(n_ch);
  UNUSED(ch);
  UNUSED(ret);
}
void nr_mac_rrc_inactivity_timer_ind(const module_id_t mod_id)
{
  UNUSED(mod_id);
}
tbs_size_t nr_mac_rlc_data_req(const module_id_t module_idP,
                               const uint16_t ue_id,
                               const bool gnb_flagP,
                               const logical_chan_id_t channel_idP,
                               const tb_size_t tb_sizeP,
                               char *buffer_pP)
{
  UNUSED(module_idP);
  UNUSED(ue_id);
  UNUSED(gnb_flagP);
  UNUSED(channel_idP);
  UNUSED(tb_sizeP);
  UNUSED(buffer_pP);
  return 0;
}
void nr_mac_rlc_data_ind(const module_id_t module_idP,
                         const uint16_t ue_id,
                         const bool gnb_flagP,
                         const nr_rlc_data_ind_t *data,
                         int num_data)
{
  UNUSED(module_idP);
  UNUSED(ue_id);
  UNUSED(gnb_flagP);
  UNUSED(data);
  UNUSED(num_data);
}
void nr_mac_rrc_verification_failed(const module_id_t mod_id)
{
  UNUSED(mod_id);
}
bool nr_rlc_activate_srb0(int ue_id,
                          void *data,
                          void (*send_initial_ul_rrc_message)(int ue_id, const uint8_t *sdu, sdu_size_t sdu_len, void *data))
{
  UNUSED(ue_id);
  UNUSED(data);
  UNUSED(send_initial_ul_rrc_message);
  return true;
}
int nr_rlc_module_init(nr_rlc_op_mode_t mode)
{
  UNUSED(mode);
  return 0;
}
MessageDef *itti_alloc_new_message(task_id_t origin_task_id, instance_t originInstance, MessagesIds message_id)
{
  UNUSED(origin_task_id);
  UNUSED(originInstance);
  UNUSED(message_id);
  return NULL;
}
int itti_send_msg_to_task(task_id_t task_id, instance_t instance, MessageDef *message)
{
  UNUSED(task_id);
  UNUSED(instance);
  UNUSED(message);
  return 0;
}
typedef uint32_t channel_t;
int8_t nr_mac_rrc_data_ind_ue(const module_id_t module_id,
                              const int CC_id,
                              const uint8_t gNB_index,
                              const int hfn,
                              const frame_t frame,
                              const int slot,
                              const rnti_t rnti,
                              const uint32_t cellid,
                              const long arfcn,
                              const channel_t channel,
                              const uint8_t *pduP,
                              const sdu_size_t pdu_len)
{
  UNUSED(module_id);
  UNUSED(CC_id);
  UNUSED(gNB_index);
  UNUSED(hfn);
  UNUSED(frame);
  UNUSED(slot);
  UNUSED(rnti);
  UNUSED(cellid);
  UNUSED(arfcn);
  UNUSED(channel);
  UNUSED(pduP);
  UNUSED(pdu_len);
  return 0;
}
bool check_csi_report_consistency(const NR_CSI_MeasConfig_t *meas)
{
  UNUSED(meas);
  return true;
}
void nr_mac_rrc_meas_ind_ue(module_id_t module_id,
                            uint32_t gNB_index,
                            uint16_t Nid_cell,
                            bool csi_meas,
                            bool is_neighboring_cell,
                            int rsrp_dBm)
{
  UNUSED(module_id);
  UNUSED(gNB_index);
  UNUSED(Nid_cell);
  UNUSED(csi_meas);
  UNUSED(is_neighboring_cell);
  UNUSED(rsrp_dBm);
}
}

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <functional>
#include <random>
#include <vector>
#include "common/utils/LOG/log.h"

namespace {

constexpr module_id_t kModId = 0;
constexpr uint16_t kPci = 382;
constexpr uint16_t kBwpSize = 106; // any legal BWP width: only the DM-RS mask is asserted

/// One MIB as the PHY measured it off the air.
struct AirMib {
  int kssb;            ///< FR1 k_SSB, 0..31 (bit 4 travels in the PBCH extra bits)
  long dmrs_pos_enum;  ///< NR_MIB__dmrs_TypeA_Position_pos2 / _pos3
  long cset0_index;    ///< pdcch-ConfigSIB1.controlResourceSetZero
  long ss0_index;      ///< pdcch-ConfigSIB1.searchSpaceZero
};

/// First (lowest) DM-RS symbol the blind grant extraction derives for a type-A PDSCH, using the
/// dmrs-TypeA-Position the blind monitor is currently configured with. -1 if the extraction
/// rejected the grant. The payload is the smallest legal DCI 1_1: identifier = 1, everything else 0
/// (RIV 0 = 1 PRB at 0, default TDRA row 0 = type A, antenna-port row 0).
int FirstBlindDmrsSymbol(int *mask_out = nullptr)
{
  const nr_pdcch_blind_monitor_cfg_t *cfg = nr_pdcch_blind_monitor_get_cfg();
  const uint16_t len = nr_pdcch_blind_dci_size(kBwpSize);
  nr_pdcch_blind_raw_result_t raw = {};
  raw.payload = 1ULL << (len - 1);
  raw.rnti = 0x4601;
  nr_pdcch_blind_result_t out = {};
  if (!nr_pdcch_blind_extract_11(&raw, len, kBwpSize, (uint8_t)cfg->dmrs_typeA_position, nullptr, &out)) {
    fprintf(stderr, "blind extraction rejected the grant: %s\n", out.reject_reason ? out.reject_reason : "?");
    return -1;
  }
  if (mask_out)
    *mask_out = out.dl_dmrs_symb_pos;
  EXPECT_EQ(out.mapping_type, 0) << "row 0 of the default TDRA table is PDSCH mapping type A";
  for (int s = 0; s < 14; s++)
    if (out.dl_dmrs_symb_pos & (1 << s))
      return s;
  return -1;
}

NR_UE_MAC_INST_t *InitPassiveUe(void)
{
  memset(&softmodem_params, 0, sizeof(softmodem_params));
  softmodem_params.passive_rx = 1; // --passive-rx: never attaches, never transmits
  softmodem_params.nsa = 0;        // blind: the receiver is NOT told the cell is NSA
  NR_UE_MAC_INST_t *mac = nr_l2_init_ue(kModId, 1 /* numerology discovered at sync: 30 kHz */);
  return mac;
}

/// Deliver one MIB exactly as production does. Returns the MAC instance.
NR_UE_MAC_INST_t *DeliverMib(const AirMib &m)
{
  NR_UE_MAC_INST_t *mac = get_mac_inst(kModId);
  // What handle_bcch_bch() (NR_IF_Module.c) writes from the PHY's SSB indication before the PDU
  // goes to RRC: SSB index, PCI, the PBCH payload extra bits (bit 5 = k_SSB MSB in FR1) and the
  // SSB's start subcarrier; an L_max=8 burst makes it FR1.
  mac->mib_ssb = 0;
  mac->physCellId = kPci;
  mac->mib_additional_bits = ((m.kssb >> 4) & 1) << 5;
  mac->ssb_start_subcarrier = 24 * 12;
  mac->frequency_range = FR1;
  mac->nr_band = 78;

  // What RRC hands MAC after decoding the BCCH-BCH message.
  uint8_t sfn6 = 0x10 << 2; // 6-bit systemFrameNumber MSBs, left-aligned
  NR_MIB_t mib = {};
  mib.systemFrameNumber.buf = &sfn6;
  mib.systemFrameNumber.size = 1;
  mib.systemFrameNumber.bits_unused = 2;
  mib.subCarrierSpacingCommon = NR_MIB__subCarrierSpacingCommon_scs30or120;
  mib.ssb_SubcarrierOffset = m.kssb & 0xF;
  mib.dmrs_TypeA_Position = m.dmrs_pos_enum;
  mib.pdcch_ConfigSIB1.controlResourceSetZero = m.cset0_index;
  mib.pdcch_ConfigSIB1.searchSpaceZero = m.ss0_index;
  mib.cellBarred = NR_MIB__cellBarred_notBarred;
  mib.intraFreqReselection = NR_MIB__intraFreqReselection_notAllowed;
  nr_rrc_mac_config_req_mib(kModId, 0, &mib, false);
  nr_rrc_mac_sched_sib(kModId, 1);
  EXPECT_EQ(mac->ssb_subcarrier_offset, m.kssb) << "MAC must reassemble k_SSB from MIB + PBCH extra bits";
  EXPECT_EQ(mac->dmrs_TypeA_Position, m.dmrs_pos_enum);
  return mac;
}

/// The only step between MIB and the CSS0 autoconf in production: ue_dci_configuration() calls
/// update_pdcch_config() iff mac->get_sib1 (nr_ue_dci_configuration.c). Mirror that gate.
void RunCss0GateLikeUeDciConfiguration(NR_UE_MAC_INST_t *mac)
{
  if (mac->get_sib1 || mac->update_pdcch_config) {
    update_pdcch_config(mac);
    mac->update_pdcch_config = false;
  }
}

/// No CORESET#0 was invented: MAC is not after SIB1, never derived CORESET#0/SS#0, and the blind
/// monitor holds neither a CSS0 snapshot nor a CORESET#0-typed live config.
void ExpectCss0Absent(const NR_UE_MAC_INST_t *mac)
{
  EXPECT_FALSE(mac->get_sib1);
  EXPECT_EQ(mac->coreset0, nullptr);
  EXPECT_EQ(mac->search_space_zero, nullptr);
  EXPECT_EQ(nr_pdcch_blind_monitor_css0_cfg(), nullptr);
  EXPECT_NE(nr_pdcch_blind_monitor_get_cfg()->coreset_type, 1);
}

void EnableAutoconf(void)
{
  // pdcch_blind_monitor_autoconf = 1: the operator's "discover everything yourself" switch, the mode
  // a blind receiver runs in. A mode knob, not a cell fact.
  const_cast<nr_pdcch_blind_monitor_cfg_t *>(nr_pdcch_blind_monitor_get_cfg())->autoconf = 1;
}

/// Body of one isolated scenario, run in the child: intercept the scenario's own assertion
/// failures, echo them on stderr (which the death-test harness shows as the parent's "Actual msg")
/// and turn them into the exit status.
[[noreturn]] void RunScenarioAndExit(const std::function<void()> &scenario)
{
  ::testing::TestPartResultArray results;
  {
    ::testing::ScopedFakeTestPartResultReporter rep(::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ALL_THREADS,
                                                    &results);
    scenario();
  }
  int failed = 0;
  for (int i = 0; i < results.size(); i++) {
    const ::testing::TestPartResult &r = results.GetTestPartResult(i);
    if (r.failed()) {
      failed++;
      fprintf(stderr, "%s:%d: %s\n", r.file_name() ? r.file_name() : "?", r.line_number(), r.message());
    }
  }
  exit(failed ? 1 : 0);
}

/// Run one scenario in a fresh child process (see the file comment).
#define RUN_ISOLATED(scenario) EXPECT_EXIT(RunScenarioAndExit([] { scenario; }), ::testing::ExitedWithCode(0), "")

void NsaMibScenario(long dmrs_enum, int expect_symbol)
{
  EnableAutoconf();
  NR_UE_MAC_INST_t *mac = InitPassiveUe();
  ASSERT_NE(mac, nullptr);
  DeliverMib({31, dmrs_enum, 0, 0});
  RunCss0GateLikeUeDciConfiguration(mac);

  ExpectCss0Absent(mac);
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg()->dmrs_typeA_position, dmrs_enum);
  int mask = 0;
  EXPECT_EQ(FirstBlindDmrsSymbol(&mask), expect_symbol) << "blind DM-RS mask 0x" << std::hex << mask;
}

void Css0CellScenario(long pos)
{
  EnableAutoconf();
  NR_UE_MAC_INST_t *mac = InitPassiveUe();
  ASSERT_NE(mac, nullptr);
  DeliverMib({0, pos, 12 /* CORESET#0 48 RB x 1 symbol at 30/30 kHz */, 0});
  EXPECT_TRUE(mac->get_sib1);
  RunCss0GateLikeUeDciConfiguration(mac);
  ASSERT_NE(mac->coreset0, nullptr);
  const nr_pdcch_blind_monitor_cfg_t *c0 = nr_pdcch_blind_monitor_css0_cfg();
  ASSERT_NE(c0, nullptr) << "CSS0 autoconf did not run";
  EXPECT_EQ(c0->coreset_type, 1);
  EXPECT_EQ(c0->dmrs_typeA_position, pos);
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg()->coreset_type, 1);
  EXPECT_EQ(nr_pdcch_blind_monitor_get_cfg()->dmrs_typeA_position, pos);
  EXPECT_EQ(FirstBlindDmrsSymbol(), pos == NR_MIB__dmrs_TypeA_Position_pos3 ? 3 : 2);
}

/// Blind dedicated-CORESET discovery (Technique A, nr_pdcch_blind_monitor_autodiscover_step() --
/// the call process_body() makes BEFORE its search-space periodicity gate) on one frequency-domain
/// symbol carrying PDCCH DM-RS in exactly one 6-RB window at `occupied_rb`, in noise. Returns the
/// call on which a footprint was declared, or -1. Same synthetic air as the DiscoveryGates tests in
/// nr_pdcch_blind_monitor_test.cc; the PDCCH DM-RS scrambling identity is the PCI.
int RunBlindDiscovery(int occupied_rb, int max_calls)
{
  const int n_rb_carrier = 48, ofdm_symbol_size = 512, first_carrier_offset = 10;
  const int slot = 3, symbol = 0;
  std::vector<c16_t> rxdataF(ofdm_symbol_size, {0, 0});
  std::mt19937 rng(1000 + occupied_rb);
  std::normal_distribution<double> noise(0.0, 8.0);
  for (auto &x : rxdataF) {
    x.r = (int16_t)std::lround(noise(rng));
    x.i = (int16_t)std::lround(noise(rng));
  }
  const int pilot_rb_count = occupied_rb + 6;
  uint32_t *gold = nr_gold_pdcch(n_rb_carrier, 14, kPci, slot, symbol);
  std::vector<c16_t> pilot(pilot_rb_count * 3);
  nr_pdcch_dmrs_ref(gold, pilot.data(), (unsigned short)pilot_rb_count);
  for (int rb = occupied_rb; rb < occupied_rb + 6; rb++) {
    for (int p = 0; p < 3; p++) {
      const int k = (first_carrier_offset + rb * 12 + 1 + 4 * p) % ofdm_symbol_size;
      rxdataF[k].r = (int16_t)pilot[rb * 3 + p].r;
      rxdataF[k].i = (int16_t)(-pilot[rb * 3 + p].i);
    }
  }
  for (int i = 1; i <= max_calls; i++) {
    if (nr_pdcch_blind_monitor_autodiscover_step(rxdataF.data(), ofdm_symbol_size, n_rb_carrier, first_carrier_offset, kPci,
                                                 slot, symbol, (uint32_t)i))
      return i;
  }
  return -1;
}

constexpr int kOccupiedRb = 18;
constexpr int kDiscoveryMaxCalls = 9000; // DiscoveryGates' bound: 8 dwells x 1000 calls + margin

/// NSA MIB, then blind discovery with NO CSS0 ever configured. Returns the MAC instance.
NR_UE_MAC_INST_t *NsaMibThenBlindDiscovery(void)
{
  EnableAutoconf();
  // pdcch_blind_monitor_autodiscover = 1: mode knob, as EnableAutoconf().
  const_cast<nr_pdcch_blind_monitor_cfg_t *>(nr_pdcch_blind_monitor_get_cfg())->autodiscover = 1;
  NR_UE_MAC_INST_t *mac = InitPassiveUe();
  EXPECT_NE(mac, nullptr);
  if (!mac)
    return nullptr;
  DeliverMib({31, NR_MIB__dmrs_TypeA_Position_pos3, 0, 0});
  RunCss0GateLikeUeDciConfiguration(mac);
  ExpectCss0Absent(mac);
  // What rt.c:process_body() checks before calling the discovery step: the monitor is enabled and
  // discovery is neither done nor paused. (In the softmodem, init() enables the monitor when autoconf is
  // set without manual geometry; this test drives the discovery step directly and does not call init().)
  EXPECT_FALSE(nr_pdcch_blind_monitor_autodiscover_done());
  EXPECT_FALSE(nr_pdcch_blind_monitor_discovery_paused());
  const int calls = RunBlindDiscovery(kOccupiedRb, kDiscoveryMaxCalls);
  EXPECT_GT(calls, 0) << "blind discovery did not converge without CSS0";
  return mac;
}

/// Step-3 verdict: blind dedicated-CORESET discovery converges on a cell that has NO CORESET#0, with
/// no CSS0 autoconf ever run, and leaves the monitor with an occasion gate that admits every slot --
/// i.e. process_body()'s own `ss_monitoring_slot_periodicity <= 0` early return no longer applies.
void NoCss0DiscoveryScenario(void)
{
  NR_UE_MAC_INST_t *mac = NsaMibThenBlindDiscovery();
  ASSERT_NE(mac, nullptr);
  const nr_pdcch_blind_monitor_cfg_t *c = nr_pdcch_blind_monitor_get_cfg();
  EXPECT_TRUE(nr_pdcch_blind_monitor_autodiscover_done());
  EXPECT_EQ(c->coreset_type, 0) << "a dedicated (PDCCH-Config) CORESET, not CORESET#0";
  EXPECT_EQ(c->coreset_rb_offset, kOccupiedRb);
  EXPECT_GE(c->ss_monitoring_slot_periodicity, 1);
  ExpectCss0Absent(mac);
  // Discovery must not have clobbered the MIB fact (nr_pdcch_blind_monitor.c: "left exactly as ...
  // set it"): grants extracted on the discovered CORESET still use DM-RS symbol 3.
  EXPECT_EQ(c->dmrs_typeA_position, NR_MIB__dmrs_TypeA_Position_pos3);
  EXPECT_EQ(FirstBlindDmrsSymbol(), 3);
}

/// Control (c) on genuinely LEARNED state: after blind discovery has converged, re-delivering the
/// MIB (every re-acquisition does) leaves the whole learned config bit-identical and does not restart
/// discovery.
void RepeatedNsaMibScenario(void)
{
  NR_UE_MAC_INST_t *mac = NsaMibThenBlindDiscovery();
  ASSERT_NE(mac, nullptr);
  ASSERT_TRUE(nr_pdcch_blind_monitor_autodiscover_done());
  const nr_pdcch_blind_monitor_cfg_t *c = nr_pdcch_blind_monitor_get_cfg();
  const nr_pdcch_blind_monitor_cfg_t learned = *c;
  const uint64_t gen = nr_pdcch_blind_monitor_autodiscover_generation();

  for (int i = 0; i < 3; i++) {
    DeliverMib({31, NR_MIB__dmrs_TypeA_Position_pos3, 0, 0});
    RunCss0GateLikeUeDciConfiguration(mac);
  }
  EXPECT_EQ(memcmp(c, &learned, sizeof(learned)), 0) << "re-delivered MIB disturbed learned blind state";
  EXPECT_EQ(nr_pdcch_blind_monitor_autodiscover_generation(), gen);
  EXPECT_TRUE(nr_pdcch_blind_monitor_autodiscover_done());
  EXPECT_EQ(c->coreset_rb_offset, kOccupiedRb);
  ExpectCss0Absent(mac);
  EXPECT_EQ(FirstBlindDmrsSymbol(), 3);
}

} // namespace

// RED before the MIB handoff: the NSA cell's MIB says pos3, the blind chain kept its pos2 default.
TEST(MibBlindHandoff, NsaKssb31Pos3BlindGrantsUseDmrsSymbol3AndNoCss0)
{
  RUN_ISOLATED(NsaMibScenario(NR_MIB__dmrs_TypeA_Position_pos3, 3));
}

// Control (a): same NSA cell, pos2 -> symbol 2.
TEST(MibBlindHandoff, NsaKssb31Pos2BlindGrantsUseDmrsSymbol2AndNoCss0)
{
  RUN_ISOLATED(NsaMibScenario(NR_MIB__dmrs_TypeA_Position_pos2, 2));
}

// Control (b): a normal cell with CORESET#0 (k_SSB 0) still configures via the existing CSS0 path,
// carrying the MIB's position.
TEST(MibBlindHandoff, Css0CellPos3StillConfiguresThroughCss0Autoconf)
{
  RUN_ISOLATED(Css0CellScenario(NR_MIB__dmrs_TypeA_Position_pos3));
}

TEST(MibBlindHandoff, Css0CellPos2StillConfiguresThroughCss0Autoconf)
{
  RUN_ISOLATED(Css0CellScenario(NR_MIB__dmrs_TypeA_Position_pos2));
}

// Control (c): once blind discovery has learned a dedicated CORESET, re-delivering the MIB leaves
// every learned field bit-identical and does not restart discovery.
TEST(MibBlindHandoff, RepeatedNsaMibLeavesLearnedBlindGeometryIntact)
{
  RUN_ISOLATED(RepeatedNsaMibScenario());
}

// Step 3: blind discovery is reachable with no CSS0 at all (NSA, no CORESET#0).
TEST(MibBlindHandoff, NsaBlindDiscoveryConvergesWithoutAnyCss0)
{
  RUN_ISOLATED(NoCss0DiscoveryScenario());
}

int main(int argc, char **argv)
{
  logInit();
  configmodule_interface_t *cfg = load_configmodule(argc, argv, CONFIG_ENABLECMDLINEONLY);
  ::testing::InitGoogleTest(&argc, argv);
  // Re-exec per scenario: every scenario starts from a pristine process (file comment).
  ::testing::GTEST_FLAG(death_test_style) = "threadsafe";
  int ret = RUN_ALL_TESTS();
  end_configmodule(cfg);
  return ret;
}
