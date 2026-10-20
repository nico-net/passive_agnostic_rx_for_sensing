"""Experiment-mode policy: which datasets/columns/values may be persisted. Fails closed.

Commercial mode may persist only aggregates: no RNTI (not even hashed), no per-grant/per-UE rows, no payload,
no raw log text. A violation raises PolicyViolation whose message never contains row values.
"""
import math

MODES = ("lab", "commercial")
K_COMMERCIAL = 10
WINDOW_S = 60

ENUMS = {
    "dir": ("dl", "ul"),
    "outcome": ("crc_ok", "crc_fail"),
    "fmt": ("1_0", "1_1", "0_x", "0_1"),
    "mcs_bucket": ("0-4", "5-9", "10-16", "17-27", "na"),
    "nl_bucket": ("1", "2", "3-4", "na"),
    "ue_bucket": ("0", "1-9", "10-49", "50+"),
    "acq_state": ("SEARCHING", "PBCH_LOCKED", "SIB1_DECODED", "PDCCH_LOCKED", "CORESET_VERIFIED", "CELL_CONFIGURED",
                  "DL_CONVERGED", "UL_CONVERGED", "TRACKING", "LOST", "INVALID", "UNKNOWN"),
    "lat_metric": ("e2e_tot", "legacy_dec_us", "legacy_pdtim_real", "arr2enq", "qwait", "pre", "dec_span", "serial"),
    "disc_event": ("sync_start", "cell_detected", "sync_ok", "pbch_ok", "pbch_locked", "sib1_decoded", "css0_autoconf",
                   "coreset_verified", "coreset_bank_add", "dci_len_locked", "bwp_resolved", "td_converged",
                   "fieldbook_promoted", "fieldbook_withdrawn", "ul_locked", "first_dci", "first_crc_ok", "acq_state"),
    "cfg_kind": ("epoch_bump", "continuity_gap"),
    "cfg_class": ("SOFT", "HARD_REVERIFY", "HARD_RESET", "UNKNOWN"),
    "cfg_cause": ("CELL_IDENTITY_CHANGE", "MIB_CHANGE", "SIB1_CHANGE", "SI_MODIFICATION_ANNOUNCED", "CONTINUITY_LOSS",
                  "BWP_CHANGE", "DEDICATED_CHANGE_SUSPECTED", "CSIRS_MAP_CHANGE", "RX_RECOVERY", "UNKNOWN"),
    "retry_kind": ("recovlate", "ul_retry", "layout_probe"),
    "health_kind": ("rxdiscont", "rftsdisc", "rfcensus", "usrp_ring_recover", "usrp_ring_restart", "rfstall",
                    "rx_recover"),
    "src": ("host", "metrics"),
}

# kind: int | float | bool | str(enum name); cls: "agg" (safe aggregate/meta) or "ident" (identifies a UE)
def _c(kind, cls="agg"):
    return (kind, cls)

_I, _F, _B = _c("int"), _c("float"), _c("bool")
_E = lambda name: _c("enum:" + name)
_RNTI = _c("int", "ident")

METRIC_INT = (
    "pci acq_transitions acq_sync_losses acq_pbch_locks acq_sib1_decodes pdcch_occasions pdcch_candidates pdcch_accepts "
    "pdcch_accepts_c scanq_queued scanq_processed scanq_drop_full scanq_drop_stale scanq_drop_epoch scanq_max_lag "
    "pdschq_queued pdschq_decoded pdschq_crc_ok pdschq_drop_full pdschq_drop_stale pdschq_drop_epoch "
    "pdschq_stale_after_decode pdschq_max_lag puschq_drop_epoch pusch_try pusch_crc_ok ldpc_ok ldpc_seg_fail "
    "ldpc_tb_fail ldpc_zero_tb discovery_yields discovery_overruns rt_gpu_fallbacks abs_slot t_mono_ns t_real_ns "
    "cfg_epoch cfg_identity_gen proc_utime_us proc_stime_us proc_maxrss_kb proc_minflt proc_majflt").split()
METRIC_FLOAT = ("gpu_budget_used_pct",)
# receiver keys added by the C side: any numeric recov_* / retry_yield_* is accepted, no code change needed
DYN_PREFIXES = ("recov_", "retry_yield_")

_SYS = {"t_s": _F, "src": _E("src"), "acq_state": _E("acq_state"), "host_cpu_pct": _F, "mem_avail_kb": _I,
        "rx_cpu_pct": _F, "rx_rss_kb": _I, "rx_threads": _I, "gpu_util_pct": _F, "gpu_mem_mb": _F, "gpu_temp_c": _F}
_SYS.update({k: _I for k in METRIC_INT})
_SYS.update({k: _F for k in METRIC_FLOAT})

_GRANT_US = {k: _I for k in ("legacy_dec_us", "e2e_tot_us", "arr2enq_us", "qwait_us", "pre_us", "dec_span_us", "serial_us",
                             "first_body_us", "fep_us", "chest_us", "demod_us", "ldpc_us", "es_lead_us")}

DATASETS = {
    "rx_grants": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "ext_slot": _I, "frame": _I, "slot": _I, "rnti": _RNTI, "dir": _E("dir"),
        "outcome": _E("outcome"), "probe": _B, "mcs": _I, "tbl": _I, "qm": _I, "nl": _I, "rb_start": _I, "rb_n": _I,
        "nsym": _I, "snr_db": _F, "evm_pct": _F, "rv": _I, "ndi": _I, "harq_pid": _I, "tbs": _I, "nbrx": _I,
        "early": _B, "bodies": _I, "lag_slots": _I, "span_inconsistent": _B, **_GRANT_US}},
    "dci_attempts": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "ext_slot": _I, "frame": _I, "slot": _I, "rnti": _RNTI, "dir": _E("dir"),
        "fmt": _E("fmt"), "cce": _I, "al": _I, "utc_ns": _I}},
    "discovery_events": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "event": _E("disc_event"), "pci": _I, "rnti": _RNTI, "ival": _I, "ival2": _I,
        "state_from": _E("acq_state"), "state_to": _E("acq_state")}},
    "configuration_events": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "kind": _E("cfg_kind"), "epoch_from": _I, "epoch_to": _I, "cls": _E("cfg_class"),
        "cause": _E("cfg_cause"), "gap_samples": _I, "gap_ms": _F}},
    "retry_events": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "kind": _E("retry_kind"), "dir": _E("dir"), "rnti": _RNTI, "frame": _I, "slot": _I,
        "ext_slot": _I, "harq_pid": _I, "tbs": _I, "recovered_cfg": _B, "tot_us": _I, "attributed": _B, "orig_seq": _I,
        "cdm": _I, "s": _I, "l": _I, "n": _I, "cb0_ok": _I}},
    "slot_health": {"modes": ("lab",), "cols": {
        "seq": _I, "t_s": _F, "kind": _E("health_kind"), "abs_slot": _I, "frame": _I, "slot": _I, "delta": _I, "n": _I,
        "pbch_ok": _I, "pbch_fail": _I, "slots": _I, "attempt": _I, "gap_samples": _I, "resync_ms": _I}},
    "system_telemetry": {"modes": ("lab", "commercial"), "cols": _SYS},
    "latency_histograms": {"modes": ("lab", "commercial"), "cols": {
        "window_start_s": _I, "pci": _I, "dir": _E("dir"), "metric": _E("lat_metric"), "bucket_lo_us": _I,
        "bucket_hi_us": _I, "count": _I}},
    "cell_aggregates": {"modes": ("lab", "commercial"), "cols": {
        "window_start_s": _I, "pci": _I, "dir": _E("dir"), "mcs_bucket": _E("mcs_bucket"), "nl_bucket": _E("nl_bucket"),
        "n_grants": _I, "n_crc_ok": _I, "n_crc_fail": _I, "snr_db_mean": _F, "e2e_tot_p50_us": _F,
        "e2e_tot_p99_us": _F, "legacy_dec_p50_us": _F, "legacy_dec_p99_us": _F}},
    "discovery_summary": {"modes": ("lab", "commercial"), "cols": {
        "window_start_s": _I, "pci": _I, "acq_state": _E("acq_state"), "ue_bucket": _E("ue_bucket"),
        "n_dci_accepts": _I, "n_grants": _I, "n_crc_ok": _I}},
}


class PolicyViolation(Exception):
    """Never carries row values or input text; n_fields = offending columns in the refused row."""
    def __init__(self, msg, n_fields=1):
        super().__init__(msg)
        self.n_fields = n_fields


def datasets_for(mode):
    return tuple(d for d, s in DATASETS.items() if mode in s["modes"])


def column_spec(dataset, col):
    spec = DATASETS[dataset]["cols"].get(col)
    if spec is None and dataset == "system_telemetry" and col.startswith(DYN_PREFIXES):
        spec = _F
    return spec


def validate_row(mode, dataset, row):
    """Raises PolicyViolation; returns None when the row may be persisted."""
    if mode not in MODES:
        raise PolicyViolation("unknown mode")
    spec = DATASETS.get(dataset)
    if spec is None:
        raise PolicyViolation("unknown dataset")
    if mode not in spec["modes"]:
        raise PolicyViolation("dataset %s not permitted in %s mode" % (dataset, mode))
    bad = 0
    for col, val in row.items():
        cs = column_spec(dataset, col)
        if cs is None or (mode == "commercial" and cs[1] != "agg"):
            bad += 1
            continue
        kind = cs[0]
        if val is None:
            continue
        if kind == "int":
            ok = isinstance(val, int) and not isinstance(val, bool) and -(1 << 63) <= val < (1 << 63)
        elif kind == "float":
            ok = isinstance(val, (int, float)) and not isinstance(val, bool) and math.isfinite(val)
        elif kind == "bool":
            ok = isinstance(val, bool)
        else:
            ok = isinstance(val, str) and val in ENUMS[kind[5:]]
        bad += not ok
    if bad:
        raise PolicyViolation("row refused by policy (dataset %s)" % dataset, bad)
