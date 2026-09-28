### Task 16: Blind CSI-RS rows 6–18 (8–32 ports)

Commercial massive-MIMO cells use 8–32-port CSI-RS for CSI acquisition; enumeration stops at row 5. OAI already implements TS 38.211 Table 7.4.1.5.3-1 for every row: `get_csi_mapping_parms(row, b, l0, l1)` (`PHY/nr_phy_common/inc/nr_phy_common.h:333`) — reuse it, do not transcribe the table.

**Files:**
- Modify: `openair1/PHY/NR_UE_TRANSPORT/nr_csirs_blind_search.{h,c}`, `nr_csirs_blind_rt.c`, `tests/nr_csirs_blind_search_test.cc`

- [ ] **Step 1: Failing tests.** `RowPortsMatchTheSpecTableAllRows`: extend `nr_csirs_blind_row_ports` to rows 1–18 = {1,1,2,4,4,8,8,8,12,12,16,16,24,24,24,32,32,32}. `EveryRowsFootprintHasPortsRes`: for each row 6–18 and a legal bitmap/l0/l1, `get_csi_mapping_parms` yields `ports × density` REs per RB (density 1 → ports REs).
  This deliberately changes Task 2's `EXPECT_EQ(nr_csirs_blind_row_ports(6), 0)` to 8 (spec extension, not a relaxation); keep `kRows` as the round-robin set and give `nr_csirs_blind_row_ports` its own 18-entry table.
- [ ] **Step 2: Footprint-first search.** Enumerating every bitmap × l0 × l1 for rows 6–18 is too large to round-robin. Use the existing EPR detector (memory `blind-csirs-oracle-was-wrong-three-ways`: EPR finds the TRS pair reproducibly) to measure which (subcarrier-in-RB, symbol) REs carry periodic energy; enumerate only (row, bitmap, l0, l1) whose `get_csi_mapping_parms` footprint equals the measured one; feed those into the existing confirm + IDSWEEP path.
- [ ] **Step 3:** Build + ctest; commit `Blind CSI-RS: rows 6-18 via EPR footprint matching over OAI's own mapping table`.

---

