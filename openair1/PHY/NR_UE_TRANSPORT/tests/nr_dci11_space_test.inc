// DCI11-RESOLVER (2026-10-24): coverage of the TS 38.212 7.3.1.2.2 (DCI format 1_1) field space by the layout resolver.
// Included at the end of nr_pdcch_dci11_layout_sweep_test.cc (kPci415Dci11, riv_bits_for, put_bits are defined there).
//
// The resolver groups fields into sums (nr_pdcch_dci11_layout_sweep.c). This file rebuilds the same space FIELD BY FIELD from
// the standard's table below -- one entry per field, every width the standard allows, in payload order, with the couplings
// the field definitions state -- by a dynamic program over bit positions, independent of the resolver's grouping. A field
// option the resolver does not enumerate makes Dci11Space.FieldByFieldSpaceEqualsTheResolver fail with the missing layout;
// an option the standard does not allow makes it fail with the extra one.
#include <algorithm>
#include <array>
#include <string>
#include <map>
#include <set>
#include <tuple>

namespace dci11space {

// What a field is, for the reader: the read fields record their offsets; every other field only moves the position.
enum Kind { PLAIN, ID, FDRA, TDA, TB1, TB2, TB2X, ANT, CBGTI, CBGFI, DMRS_INIT, PAD };
struct Field {
  const char *name;
  Kind kind;
  std::vector<int> widths;
};
// TS 38.212 V17.13.0 (2025-12) clause 7.3.1.2.2, DCI format 1_1, FR1: one entry per field heading of the clause, in payload order,
// with the SPEC'S OWN NAMES (Dci11Space.TableFollowsTheSpecText compares them with the generated nr_dci11_spec_fields.inc). Widths
// the clause allows that are left out: TDRA 5-6 bits and NDI 2-8 bits exist only with pdsch-TimeDomainAllocationListForMultiPDSCH
// (multi-PDSCH scheduling, FR2-2). Couplings: VRB-to-PRB is 0 bits under RA type 0 only; transport block 2 (MCS/NDI/RV) is present
// only with maxNrofCodeWordsScheduledByDCI = 2; CBGTI is N_TB x {0,2,4,6,8}; CBGFI exists only with CBG transmission. The last
// entry is not a field: the zero padding of 7.3.1.0 (1 bit) and of the multi-search-space alignment of 7.3.1.2.2 (<= 3 more).
static const std::vector<Field> &table()
{
  static const std::vector<Field> t = {
      {"Identifier for DCI formats", ID, {1}},
      {"Carrier indicator", PLAIN, {0, 3}},
      {"Bandwidth part indicator", PLAIN, {0, 1, 2}},
      {"Frequency domain resource assignment", FDRA, {}},
      {"Time domain resource assignment", TDA, {0, 1, 2, 3, 4}},
      {"VRB-to-PRB mapping", PLAIN, {0, 1}},
      {"PRB bundling size indicator", PLAIN, {0, 1}},
      {"Rate matching indicator", PLAIN, {0, 1, 2}},
      {"ZP CSI-RS trigger", PLAIN, {0, 1, 2}},
      {"Modulation and coding scheme", TB1, {5}},
      {"New data indicator", PLAIN, {1}},
      {"Redundancy version", PLAIN, {2}},
      {"Modulation and coding scheme", TB2, {0, 5}},
      {"New data indicator", TB2X, {0, 1}},
      {"Redundancy version", TB2X, {0, 2}},
      {"HARQ process number", PLAIN, {4, 5}},
      {"Downlink assignment index", PLAIN, {0, 2, 4, 6}},
      {"TPC command for scheduled PUCCH", PLAIN, {2}},
      {"Second TPC command for scheduled PUCCH", PLAIN, {0, 2}},
      {"PUCCH resource indicator", PLAIN, {3}},
      {"PDSCH-to-HARQ_feedback timing indicator", PLAIN, {0, 1, 2, 3}},
      {"One-shot HARQ-ACK request", PLAIN, {0, 1}},
      {"Enhanced Type 3 codebook indicator", PLAIN, {0, 1, 2, 3}},
      {"PDSCH group index", PLAIN, {0, 1}},
      {"New feedback indicator", PLAIN, {0, 1, 2}},
      {"Number of requested PDSCH group(s)", PLAIN, {0, 1}},
      {"HARQ-ACK retransmission indicator", PLAIN, {0, 1}},
      {"Antenna port(s)", ANT, {4, 5, 6}},
      {"Transmission configuration indication", PLAIN, {0, 3}},
      {"SRS request", PLAIN, {2, 3}},
      {"SRS offset indicator", PLAIN, {0, 1, 2}},
      {"CBG transmission information (CBGTI)", CBGTI, {0, 2, 4, 6, 8}},
      {"CBG flushing out information (CBGFI)", CBGFI, {0, 1}},
      {"DMRS sequence initialization", DMRS_INIT, {1}},
      {"Priority indicator", PLAIN, {0, 1}},
      {"ChannelAccess-CPext", PLAIN, {0, 1, 2, 3, 4}},
      {"Minimum applicable scheduling offset indicator", PLAIN, {0, 1}},
      {"SCell dormancy indication", PLAIN, {0, 1, 2, 3, 4, 5}},
      {"PDCCH monitoring adaptation indication", PLAIN, {0, 1, 2}},
      {"PUCCH Cell indicator", PLAIN, {0, 1}},
      {"zero padding (7.3.1.0, 7.3.1.2.2 search-space alignment)", PAD, {0, 1, 2, 3, 4}},
  };
  return t;
}

// TS 38.214 Table 5.1.2.2.1-1 (RBG size P by BWP size, configurations 1 and 2), written out again here.
static int rbg_p(int N, bool cfg2)
{
  if (N <= 36) return cfg2 ? 4 : 2;
  if (N <= 72) return cfg2 ? 8 : 4;
  if (N <= 144) return cfg2 ? 16 : 8;
  return 16;
}
struct Mode { int mode, n_rbg, width; };
// The FDRA modes of a BWP: resourceAllocation (type 0 / type 1 / dynamicSwitch) x rbg-Size. rbg-Size config 2 with the same
// P as config 1 reads the same bits the same way: one mode, not two.
static std::vector<Mode> modes(int start, int N)
{
  const int riv = riv_bits_for((uint16_t)N);
  std::vector<Mode> m;
  m.push_back({NR_FDRA_TYPE1, 0, riv});
  for (int c2 = 0; c2 < 2; c2++) {
    const int P = rbg_p(N, c2);
    if (c2 && P == rbg_p(N, false)) continue;
    const int n_rbg = (N + start % P + P - 1) / P;
    m.push_back({c2 ? NR_FDRA_TYPE0_CFG2 : NR_FDRA_TYPE0_CFG1, n_rbg, n_rbg});
    m.push_back({c2 ? NR_FDRA_DYN_CFG2 : NR_FDRA_DYN_CFG1, n_rbg, 1 + std::max(n_rbg, riv)});
  }
  return m;
}

// A layout as the reader sees it: the offsets of the read fields (and the DM-RS init bit, kept apart).
using Key = std::tuple<int, int, int, int, int, int, int, int, int>; // mode, n_rbg, riv, tda, tda_bits, mcs, ant, ant_bits, dmrs_type
struct St {
  int pos, mode, n_rbg, riv, tda, tdab, mcs, ant, antb, dt, dmrs, tb2, cbg;
  bool operator<(const St &o) const
  {
    return std::tie(pos, mode, n_rbg, riv, tda, tdab, mcs, ant, antb, dt, dmrs, tb2, cbg)
           < std::tie(o.pos, o.mode, o.n_rbg, o.riv, o.tda, o.tdab, o.mcs, o.ant, o.antb, o.dt, o.dmrs, o.tb2, o.cbg);
  }
};
// Every layout of the standard at (BWP start, size, length L): key -> the set of DM-RS init positions it admits.
static std::map<Key, std::set<int>> space(int start, int N, int L)
{
  std::set<St> cur;
  for (const Mode &m : modes(start, N)) cur.insert(St{0, m.mode, m.n_rbg, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0});
  const auto mw = modes(start, N);
  for (const Field &f : table()) {
    std::set<St> nxt;
    for (const St &s : cur) {
      std::vector<int> w = f.widths;
      if (f.kind == FDRA)
        for (const Mode &m : mw)
          if (m.mode == s.mode) w = {m.width};
      for (int wi : w) {
        if (f.kind == ANT) {
          for (int dt = 0; dt < 2; dt++) { /* width 4: type 1 len 1; 5: type 1 len 2 or type 2 len 1; 6: type 2 len 2 */
            if ((wi == 4 && dt) || (wi == 6 && !dt)) continue;
            St t = s;
            t.ant = s.pos; t.antb = wi; t.dt = dt; t.pos += wi;
            if (t.pos <= L) nxt.insert(t);
          }
          continue;
        }
        St t = s;
        const bool type0 = s.mode == NR_FDRA_TYPE0_CFG1 || s.mode == NR_FDRA_TYPE0_CFG2;
        if (!strcmp(f.name, "VRB-to-PRB mapping") && type0 && wi) continue;
        int width = wi;
        switch (f.kind) {
          case FDRA: t.riv = s.pos; break;
          case TDA: t.tda = s.pos; t.tdab = wi; break;
          case TB1: t.mcs = s.pos; break;
          case TB2: t.tb2 = wi > 0; break;
          case TB2X: if ((wi > 0) != (s.tb2 != 0)) continue; break;
          case CBGTI: width = wi * (s.tb2 ? 2 : 1); t.cbg = wi > 0; break;
          case CBGFI: if (wi && !s.cbg) continue; break;
          case DMRS_INIT: t.dmrs = s.pos; break;
          default: break;
        }
        t.pos += width;
        if (f.kind != TB2 && f.kind != CBGTI && f.kind != CBGFI && t.dmrs < 0) { /* keep flags only while they matter */ }
        if (f.kind == CBGFI) { t.tb2 = 0; t.cbg = 0; }
        if (t.pos <= L) nxt.insert(t);
      }
    }
    cur.swap(nxt);
  }
  std::map<Key, std::set<int>> out;
  for (const St &s : cur)
    if (s.pos == L) out[Key{s.mode, s.n_rbg, s.riv, s.tda, s.tdab, s.mcs, s.ant, s.antb, s.dt}].insert(s.dmrs);
  return out;
}
static Key key_of(const nr_dci11_resolver_t &r, int i)
{
  const nr_dci11_offsets_t &o = r.off[i];
  return Key{o.fdra_mode, o.n_rbg, o.riv, o.tda, o.tda_bits, o.mcs, o.ant_ports, o.ant_ports_bits, r.hyp[i].dmrs_type};
}

} // namespace dci11space

// The standard space rebuilt field by field equals the resolver's set exactly, for the geometries of the cells this receiver
// has met (Iliad 51 PRB / 48 bits, Vodafone 217 / 49, TIM 217 / 47, the OAI rfsim gNB 106 / 46 and 273 / 49) and a
// misaligned BWP start. Each resolver layout also carries the LATEST DM-RS init position the standard admits for it.
TEST(Dci11Space, FieldByFieldSpaceEqualsTheResolver) {
  struct G { int start, N, L; const char *who; };
  const G geoms[] = {{0, 51, 48, "Iliad PCI 403"}, {0, 217, 49, "Vodafone PCI 415"}, {0, 217, 47, "TIM PCI 923"},
                     {0, 106, 46, "OAI rfsim 106"}, {0, 273, 49, "OAI rfsim 273"}, {15, 106, 44, "misaligned 106"},
                     {0, 24, 36, "narrow 24"}};
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  for (const G &g : geoms) {
    const auto want = dci11space::space(g.start, g.N, g.L);
    const int n = nr_dci11_resolver_init_fdra(&r, (uint16_t)g.start, (uint16_t)g.N, riv_bits_for((uint16_t)g.N), NR_DCI11_TDA_UNKNOWN,
                                              (uint16_t)g.L);
    ASSERT_EQ(r.n_truncated, 0u) << g.who;
    std::map<dci11space::Key, int> have;
    for (int i = 0; i < n; i++) {
      const auto k = dci11space::key_of(r, i);
      EXPECT_EQ(have.count(k), 0u) << g.who << ": duplicate layout " << i;
      have[k] = i;
      auto it = want.find(k);
      if (it == want.end()) {
        ADD_FAILURE() << g.who << ": layout " << i << " (mode " << std::get<0>(k) << " riv@" << std::get<2>(k) << " tda@" << std::get<3>(k)
                      << "/" << std::get<4>(k) << " mcs@" << std::get<5>(k) << " ant@" << std::get<6>(k) << "/" << std::get<7>(k)
                      << ") is not a layout of the standard";
        continue;
      }
      EXPECT_EQ(r.off[i].dmrs_init, *it->second.rbegin()) << g.who << ": layout " << i << " DM-RS init not the latest allowed";
    }
    int missing = 0;
    for (const auto &kv : want)
      if (!have.count(kv.first) && missing++ < 5)
        ADD_FAILURE() << g.who << ": the standard allows mode " << std::get<0>(kv.first) << " riv@" << std::get<2>(kv.first) << " tda@"
                      << std::get<3>(kv.first) << "/" << std::get<4>(kv.first) << " mcs@" << std::get<5>(kv.first) << " ant@"
                      << std::get<6>(kv.first) << "/" << std::get<7>(kv.first) << " type " << std::get<8>(kv.first)
                      << " -- the resolver does not enumerate it";
    EXPECT_EQ(missing, 0) << g.who;
    EXPECT_EQ((size_t)n, want.size()) << g.who;
    std::cerr << "[ MEASURED ] " << g.who << " (" << g.N << " PRB @" << g.start << ", " << g.L << " bits): " << n
              << " layouts = the field-by-field standard space\n";
  }
}

// One field at a time, at every width it can take, on top of the plainest configuration (and the same on top of a busy one):
// the resulting length's resolver set holds that exact layout. A width the resolver forgot shows up here by name.
TEST(Dci11Space, EveryWidthOfEveryFieldIsEnumerated) {
  const auto &T = dci11space::table();
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  const int N = 106, start = 0;
  const auto mw = dci11space::modes(start, N);
  for (int base = 0; base < 2; base++)
    for (size_t fi = 0; fi < T.size(); fi++)
      for (int w : T[fi].widths) {
        for (const auto &m : mw) {
          std::vector<int> cfg(T.size());
          for (size_t k = 0; k < T.size(); k++) { /* base 0: every field at its smallest width; base 1: a typical commercial set */
            cfg[k] = T[k].widths.empty() ? 0 : T[k].widths[0];
            if (base) {
              static const std::map<std::string, int> typical = {{"BWP indicator", 1}, {"TDRA", 2}, {"DAI", 2},
                                                                 {"PDSCH-to-HARQ timing", 3}, {"SRS request", 2}};
              auto it = typical.find(T[k].name);
              if (it != typical.end()) cfg[k] = it->second;
            }
          }
          cfg[fi] = w;
          const bool type0 = m.mode == NR_FDRA_TYPE0_CFG1 || m.mode == NR_FDRA_TYPE0_CFG2;
          int pos = 0, riv = 0, tda = 0, tdab = 0, mcs = 0, ant = 0, antb = 0, dmrs = 0, tb2 = 0, cbg = 0;
          bool legal = true;
          for (size_t k = 0; k < T.size(); k++) {
            int width = cfg[k];
            switch (T[k].kind) {
              case dci11space::FDRA: riv = pos; width = m.width; break;
              case dci11space::TDA: tda = pos; tdab = width; break;
              case dci11space::TB1: mcs = pos; break;
              case dci11space::TB2: tb2 = width > 0; break;
              case dci11space::TB2X: width = tb2 ? T[k].widths.back() : 0; break; /* present iff TB2 is */
              case dci11space::ANT: ant = pos; antb = width; break;
              case dci11space::CBGTI: cbg = width > 0; width *= tb2 ? 2 : 1; break;
              case dci11space::CBGFI: legal &= !(width && !cbg); break;
              case dci11space::DMRS_INIT: dmrs = pos; break;
              default: break;
            }
            if (!strcmp(T[k].name, "VRB-to-PRB mapping") && type0 && width) legal = false;
            pos += width;
          }
          if (!legal) continue;
          const int L = pos;
          if (nr_dci11_layout_space_count(start, N, riv_bits_for(N), NR_DCI11_TDA_UNKNOWN, (uint16_t)L) > NR_DCI11_LAYOUT_MAX) continue;
          const int n = nr_dci11_resolver_init_fdra(&r, start, N, riv_bits_for(N), NR_DCI11_TDA_UNKNOWN, (uint16_t)L);
          bool found = false;
          for (int i = 0; i < n && !found; i++) {
            const nr_dci11_offsets_t &o = r.off[i];
            found = o.fdra_mode == m.mode && o.riv == riv && o.tda == tda && o.tda_bits == tdab && o.mcs == mcs && o.ant_ports == ant
                    && o.ant_ports_bits == antb && o.dmrs_init >= dmrs;
          }
          EXPECT_TRUE(found) << "field '" << T[fi].name << "' at " << w << " bits (mode " << m.mode << ", base " << base << ", L " << L
                             << ") is not enumerated";
        }
      }
}

// Random configurations: every field drawn from its widths (couplings respected), several BWPs. 400 draws.
TEST(Dci11Space, RandomFieldCombinationsAreEnumerated) {
  const auto &T = dci11space::table();
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  unsigned seed = 2026;
  int tested = 0, skipped = 0;
  const int bwps[][2] = {{0, 24}, {0, 51}, {3, 51}, {0, 106}, {0, 217}, {0, 273}};
  for (int draw = 0; draw < 400; draw++) {
    const int start = bwps[draw % 6][0], N = bwps[draw % 6][1];
    const auto mw = dci11space::modes(start, N);
    const auto &m = mw[rand_r(&seed) % mw.size()];
    const bool type0 = m.mode == NR_FDRA_TYPE0_CFG1 || m.mode == NR_FDRA_TYPE0_CFG2;
    int pos = 0, riv = 0, tda = 0, tdab = 0, mcs = 0, ant = 0, antb = 0, dt = 0, tb2 = 0, cbg = 0;
    for (const auto &f : T) {
      int width = f.widths.empty() ? 0 : f.widths[rand_r(&seed) % f.widths.size()];
      switch (f.kind) {
        case dci11space::FDRA: riv = pos; width = m.width; break;
        case dci11space::TDA: tda = pos; tdab = width; break;
        case dci11space::TB1: mcs = pos; break;
        case dci11space::TB2: tb2 = width > 0; break;
        case dci11space::TB2X: width = tb2 ? f.widths.back() : 0; break;
        case dci11space::ANT: ant = pos; antb = width; dt = width == 6 ? 1 : width == 4 ? 0 : (int)(rand_r(&seed) & 1); break;
        case dci11space::CBGTI: cbg = width > 0; width *= tb2 ? 2 : 1; break;
        case dci11space::CBGFI: if (!cbg) width = 0; break;
        default: break;
      }
      if (!strcmp(f.name, "VRB-to-PRB mapping") && type0) width = 0;
      pos += width;
    }
    if (nr_dci11_layout_space_count(start, N, riv_bits_for(N), NR_DCI11_TDA_UNKNOWN, (uint16_t)pos) > NR_DCI11_LAYOUT_MAX) {
      skipped++;
      continue;
    }
    const int n = nr_dci11_resolver_init_fdra(&r, start, N, riv_bits_for(N), NR_DCI11_TDA_UNKNOWN, (uint16_t)pos);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
      const nr_dci11_offsets_t &o = r.off[i];
      found = o.fdra_mode == m.mode && o.riv == riv && o.tda == tda && o.tda_bits == tdab && o.mcs == mcs && o.ant_ports == ant
              && o.ant_ports_bits == antb && r.hyp[i].dmrs_type == dt;
    }
    EXPECT_TRUE(found) << "draw " << draw << ": " << N << " PRB mode " << m.mode << " L " << pos;
    tested++;
  }
  std::cerr << "[ MEASURED ] random field combinations: " << tested << " found, " << skipped << " over capacity (see the next test)\n";
}

// Over capacity: the evidence-filtered init keeps the truth (it reads none of the evidence as impossible) and reports the rest.
TEST(Dci11Space, OverCapacityKeepsTheTruthWithEvidence) {
  const int N = 106, L = 76;
  const uint16_t rb = riv_bits_for(N);
  const long sp = nr_dci11_layout_space_count(0, N, rb, NR_DCI11_TDA_UNKNOWN, L);
  ASSERT_GT(sp, NR_DCI11_LAYOUT_MAX) << "pick a longer length: this one fits";
  /* truth: type 1, CI 3 + BWP 2, TDA 4 bits, VRB/bundling/RM 2/ZP 2, HARQ 5 + DAI 4 + 2nd TPC + one-shot, 6-bit ports,
   * TCI + SUL SRS + SRS offset 2 + CBGTI 4 */
  nr_dci11_layout_t l{};
  l.bwp_ind = 5; l.pre_mcs = 6; l.pre_ant = 5 + 4 + 2 + 2 + 3 + 3 + 1; l.ant_ports = 6; l.dmrs_type = 1;
  l.post_ant = 3 + 3 + 2 + 4; l.fdra_mode = NR_FDRA_TYPE1;
  nr_dci11_offsets_t t{};
  ASSERT_TRUE(nr_dci11_layout_offsets(&l, rb, 4, &t));
  ASSERT_EQ(t.total, L);
  unsigned seed = 4;
  std::vector<nr_dci_bits_t> ev;
  for (int k = 0; k < 16; k++) {
    nr_dci_bits_t p{};
    for (int b = 0; b < L; b++) if (rand_r(&seed) & 1) p.w[b >> 6] |= 1ULL << (b & 63);
    auto put = [&](int off, int nb, uint32_t v) {
      for (int b = 0; b < nb; b++) {
        const int pos = L - off - nb + b;
        const uint64_t mk = 1ULL << (pos & 63);
        if ((v >> b) & 1) p.w[pos >> 6] |= mk; else p.w[pos >> 6] &= ~mk;
      }
    };
    put(0, 1, 1);
    put(t.riv, rb, rand_r(&seed) % (N * (N + 1) / 2));
    put(t.mcs, 5, rand_r(&seed) % 28);
    put(t.ant_ports, 6, rand_r(&seed) % 24);
    ev.push_back(p);
  }
  static nr_dci11_resolver_t r;
  const int n = nr_dci11_resolver_init_ev(&r, 0, N, rb, NR_DCI11_TDA_UNKNOWN, L, ev.data(), (int)ev.size());
  ASSERT_GT(n, 0);
  bool found = false;
  for (int i = 0; i < n && !found; i++)
    found = r.off[i].fdra_mode == t.fdra_mode && r.off[i].riv == t.riv && r.off[i].tda == t.tda && r.off[i].tda_bits == 4
            && r.off[i].mcs == t.mcs && r.off[i].ant_ports == t.ant_ports && r.off[i].ant_ports_bits == 6;
  EXPECT_TRUE(found) << "the evidence filter dropped a layout the evidence does not refute";
  EXPECT_EQ(r.n_space, (uint32_t)sp);
  std::cerr << "[ MEASURED ] 106 PRB / " << L << " bits: standard space " << sp << ", stored " << n << " after 16 DCIs of evidence, "
            << r.n_truncated << " not stored\n";
}

// The real payloads of the cells met on air hold their layout, and none of it reads impossible: Iliad (hand-decoded, OTA
// 2026-10-23), Vodafone (field statistics + HARQ counter, replay raw_pci415_12s_20261008), TIM (CRC-proven, replay
// raw_pci923_tim_12s_tile3 / OTA RMDCI), and the OAI gNB of the rfsim beds (its own DCI11_WIDTHS log line).
TEST(Dci11Space, RealPayloadsKeepTheirLayout) {
  auto bits = [](const char *s) {
    nr_dci_bits_t b{};
    const int n = (int)strlen(s);
    for (int i = 0; i < n; i++)
      if (s[i] == '1') b.w[(n - 1 - i) >> 6] |= UINT64_C(1) << ((n - 1 - i) & 63);
    return b;
  };
  struct Cell {
    const char *who;
    int N, L, mode, riv, tda, tdab, mcs, ant;
    std::vector<nr_dci_bits_t> p;
  };
  std::vector<nr_dci_bits_t> voda;
  for (uint64_t v : kPci415Dci11) voda.push_back(nr_dci_bits_from_u64(v));
  const std::vector<Cell> cells = {
      {"Iliad PCI 403", 51, 48, NR_FDRA_TYPE0_CFG1, 2, 15, 4, 19, 41, {nr_dci_bits_from_u64(UINT64_C(0xfffe150e2210))}},
      {"Vodafone PCI 415", 217, 49, NR_FDRA_TYPE0_CFG1, 2, 16, 0, 20, 42, voda},
      {"TIM PCI 923", 217, 47, NR_FDRA_TYPE1, 1, 16, 2, 18, 40,
       {bits("11010000000000000100001100001000010001101010000"), bits("11010000000000000000001100001000010001101010000"),
        bits("11000010000000000000000100000100010101000010000")}},
      {"OAI rfsim 106 (gNB DCI11_WIDTHS total=46)", 106, 46, NR_FDRA_TYPE1, 2, 15, 2, 17, 39, {}},
      {"OAI rfsim 273 (gNB DCI11_WIDTHS total=49)", 273, 49, NR_FDRA_TYPE1, 2, 18, 2, 20, 42, {}},
  };
  auto r_heap = std::make_unique<nr_dci11_resolver_t>();
  nr_dci11_resolver_t &r = *r_heap;
  for (const Cell &c : cells) {
    const int n = nr_dci11_resolver_init_fdra(&r, 0, c.N, riv_bits_for(c.N), NR_DCI11_TDA_UNKNOWN, c.L);
    int truth = -1, tdab = -1;
    for (int i = 0; i < n && truth < 0; i++) {
      const nr_dci11_offsets_t &o = r.off[i];
      /* Vodafone's TDA width is not known from the field statistics (4 bits between the bitmap and the MCS): any split */
      if (o.fdra_mode == c.mode && o.riv == c.riv && o.tda == c.tda && (c.tdab < 0 || c.tdab == 0 || o.tda_bits == c.tdab)
          && o.mcs == c.mcs && o.ant_ports == c.ant && o.ant_ports_bits == 4 && !r.hyp[i].dmrs_type) {
        truth = i;
        tdab = o.tda_bits;
      }
    }
    ASSERT_GE(truth, 0) << c.who << ": the layout is not enumerated";
    for (const auto &p : c.p) EXPECT_FALSE(nr_dci11_layout_impossible(&r.off[truth], p, (uint16_t)c.N)) << c.who;
    for (const auto &p : c.p) nr_dci11_resolver_observe(&r, p);
    EXPECT_TRUE(r.alive[truth]) << c.who << ": stage 1 deleted the layout";
    std::cerr << "[ MEASURED ] " << c.who << ": layout " << truth << " (tda " << tdab << " bits) of " << n << ", " << c.p.size()
              << " real payloads, " << r.n_alive << " live after them\n";
  }
}

// The enumeration count, per geometry, for the report.
TEST(Dci11Space, EnumerationCount) {
  struct G { int N, L; const char *who; };
  for (const G &g : {G{51, 48, "Iliad"}, G{217, 49, "Vodafone"}, G{217, 47, "TIM"}, G{106, 46, "rfsim 106"}, G{273, 49, "rfsim 273"}}) {
    const long sp = nr_dci11_layout_space_count(0, g.N, riv_bits_for(g.N), NR_DCI11_TDA_UNKNOWN, g.L);
    std::vector<nr_dci11_layout_t> c(1 << 16);
    long t1 = 0;
    for (uint8_t tb = 0; tb <= 4; tb++) t1 += nr_dci11_layout_enumerate(riv_bits_for(g.N), tb, g.L, c.data(), (int)c.size());
    EXPECT_GT(sp, 0);
    std::cerr << "[ MEASURED ] " << g.who << " " << g.N << " PRB / " << g.L << " bits: " << sp << " layouts (" << t1 << " RA type 1)\n";
  }
}

#include "nr_dci11_spec_fields.inc"
namespace dci11space {
// Compare a switch table with the generated spec field list: same field names in the same order (the padding entry is not a field),
// every width of the table stated by the spec (TB2's 0 = "only present if ..."), every width the spec states either in the table or
// in the FR2-2 multi-PDSCH exclusions. Returns the mismatches, empty when the table follows the text.
static std::string spec_mismatches(const std::vector<Field> &tab)
{
  std::string err;
  size_t k = 0;
  const size_t ns = sizeof(kSpec1_1) / sizeof(kSpec1_1[0]);
  for (const Field &f : tab) {
    if (f.kind == PAD) continue;
    if (k >= ns) { err += std::string("extra field '") + f.name + "'; "; continue; }
    if (strcmp(f.name, kSpec1_1[k].name) != 0)
      err += std::string("field ") + std::to_string(k) + " is '" + f.name + "', spec says '" + kSpec1_1[k].name + "'; ";
    std::set<int> spec(kSpec1_1[k].w, kSpec1_1[k].w + kSpec1_1[k].n), mine(f.widths.begin(), f.widths.end());
    const bool tb2 = f.kind == TB2 || f.kind == TB2X;
    for (int w : mine)
      if (!spec.empty() && !spec.count(w) && !(tb2 && w == 0))
        err += std::string("'") + f.name + "' width " + std::to_string(w) + " not in the spec; ";
    for (int w : spec) {
      const bool excluded = (f.kind == TDA && w >= 5) || (!strcmp(f.name, "New data indicator") && w >= 2); /* multi-PDSCH, FR2-2 */
      if (!mine.count(w) && !excluded) err += std::string("'") + f.name + "' spec width " + std::to_string(w) + " missing; ";
    }
    k++;
  }
  if (k != ns) err += "spec has " + std::to_string(ns) + " field headings, the table " + std::to_string(k) + "; ";
  return err;
}
} // namespace dci11space

// The resolver's field list is the one of TS 38.212 V17.13.0 clause 7.3.1.2.2, read from the text itself
// (tools/dci11/gen_dci11_spec_fields.py); FieldByFieldSpaceEqualsTheResolver proves the resolver enumerates exactly this table.
TEST(Dci11Space, TableFollowsTheSpecText) {
  const std::string err = dci11space::spec_mismatches(dci11space::table());
  EXPECT_TRUE(err.empty()) << err;
  /* negative controls: the check must catch a missing width, a moved field and a missing field */
  std::vector<dci11space::Field> t = dci11space::table();
  for (auto &f : t) if (!strcmp(f.name, "Enhanced Type 3 codebook indicator")) f.widths = {0, 1, 2};
  EXPECT_FALSE(dci11space::spec_mismatches(t).empty()) << "a missing width (Enhanced Type 3 = 3 bits) went unnoticed";
  t = dci11space::table();
  for (size_t i = 1; i < t.size(); i++)
    if (!strcmp(t[i].name, "PUCCH Cell indicator")) { std::swap(t[i], t[i - 1]); break; }
  EXPECT_FALSE(dci11space::spec_mismatches(t).empty()) << "a moved field went unnoticed";
  t = dci11space::table();
  for (size_t i = 0; i < t.size(); i++)
    if (!strcmp(t[i].name, "Second TPC command for scheduled PUCCH")) { t.erase(t.begin() + (long)i); break; }
  EXPECT_FALSE(dci11space::spec_mismatches(t).empty()) << "a missing field went unnoticed";
}
