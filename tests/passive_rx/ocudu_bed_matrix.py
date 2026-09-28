#!/usr/bin/env python3
"""Pure, no-radio OCUDU bed matrix plan for Round 3.

This module intentionally has no build, lock, or process-launch code. It defines
the arms and the baseline-alternated run order so those contracts can be tested
before a separately reviewed/authorized launcher is written.
"""
from dataclasses import dataclass


N_REPEATS = 3


@dataclass(frozen=True)
class Arm:
    name: str
    env: tuple[tuple[str, str], ...] = ()
    gnb_extra: str = ""
    srsue_env: tuple[tuple[str, str], ...] = ()
    route: str = "ocudu-test/srsUE"


BASELINE = Arm("baseline")
FEATURES = (
    Arm("dl_ra_type0", (("ISAC_OCUDU_TEST_DL_RA_TYPE0", "1"),)),
    Arm("ul_ra_type0", (("ISAC_OCUDU_TEST_UL_RA_TYPE0", "1"),)),
    Arm("dl_dynamic_switch", (("ISAC_OCUDU_TEST_DL_RA_TYPE0", "2"),)),
    Arm("ul_dynamic_switch", (("ISAC_OCUDU_TEST_UL_RA_TYPE0", "2"),)),
    Arm("vrb_interleaver_n2", (("ISAC_OCUDU_TEST_VRB_IL", "2"),)),
    Arm("vrb_interleaver_n4", (("ISAC_OCUDU_TEST_VRB_IL", "4"),)),
    Arm("dmrs_type2", (("ISAC_OCUDU_TEST_DMRS_TYPE2", "1"),)),
    Arm("transform_precoding", route="BLOCKED:srsUE NR PUSCH TP transmitter unavailable"),
    Arm("dl_scrambling_id0", (("ISAC_OCUDU_TEST_DL_DMRS_ID0", "700"),
                               ("ISAC_OCUDU_TEST_DL_DATA_ID", "500"),
                               ("ISAC_OCUDU_TEST_DL_DMRS_NSCID", "0"))),
    Arm("dl_scrambling_id1", (("ISAC_OCUDU_TEST_DL_DMRS_ID1", "701"),
                               ("ISAC_OCUDU_TEST_DL_DATA_ID", "501"),
                               ("ISAC_OCUDU_TEST_DL_DMRS_NSCID", "1"))),
    Arm("ul_scrambling_id0", (("ISAC_OCUDU_TEST_UL_DMRS_ID0", "700"),
                               ("ISAC_OCUDU_TEST_UL_DATA_ID", "500"),
                               ("ISAC_OCUDU_TEST_UL_DMRS_NSCID", "0"))),
    Arm("ul_scrambling_id1", (("ISAC_OCUDU_TEST_UL_DMRS_ID1", "701"),
                               ("ISAC_OCUDU_TEST_UL_DATA_ID", "501"),
                               ("ISAC_OCUDU_TEST_UL_DMRS_NSCID", "1"))),
    Arm("dl_type_b_tda", (("ISAC_OCUDU_TEST_DL_TDA", "0:typeA:2:12;0:typeB:2:4"),)),
    Arm("ul_type_b_tda", (("ISAC_OCUDU_TEST_UL_TDA", "0:typeA:2:12;0:typeB:2:4"),)),
    Arm("qam256", srsue_env=(("SRSUE_ADVERTISE_256QAM", "1"),)),
    Arm("al16", gnb_extra="cell_cfg pdcch dedicated --ss2_n_candidates 0 0 0 0 1"),
    Arm("forced_harq_retx", gnb_extra="cell_cfg pdsch --max_nof_harq_retxs 16"),
    Arm("multi_ue", route="BLOCKED:test_ue executable/config/attribution not inventoried"),
)


def make_schedule():
    """For each feature, emit B,F,B,F,B,F: paired alternating n=3 each."""
    runs = []
    for feature in FEATURES:
        for pair in range(1, N_REPEATS + 1):
            runs.append((f"{feature.name}_b{pair}", BASELINE))
            runs.append((f"{feature.name}_f{pair}", feature))
    return runs


def blocked_routes():
    return tuple(arm.name for arm in FEATURES if arm.route.startswith("BLOCKED:"))


def validate_schedule(runs=None):
    runs = make_schedule() if runs is None else runs
    expected = len(FEATURES) * N_REPEATS * 2
    if len(runs) != expected:
        raise ValueError(f"run count {len(runs)} != {expected}")
    for i in range(0, len(runs), 2):
        bname, baseline = runs[i]
        fname, feature = runs[i + 1]
        if baseline.name != "baseline" or not bname.endswith(f"_b{(i // 2) % 3 + 1}"):
            raise ValueError(f"run {i} is not the expected baseline")
        if fname.split("_f")[0] != feature.name or not fname.endswith(f"_f{(i // 2) % 3 + 1}"):
            raise ValueError(f"run {i + 1} is not its paired feature")
    return True


def main():
    validate_schedule()
    print("NO RADIO / NO BUILD: declarative plan only")
    print(f"feature_arms={len(FEATURES)} runs={len(make_schedule())} dwell_s=300")
    print("blocked_routes=" + ",".join(blocked_routes()))
    for name, arm in make_schedule():
        env = ",".join(f"{k}={v}" for k, v in arm.env + arm.srsue_env)
        print(f"{name}\t{arm.name}\t{arm.route}\t{env}\t{arm.gnb_extra}")


if __name__ == "__main__":
    main()
