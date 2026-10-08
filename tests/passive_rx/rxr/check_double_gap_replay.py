#!/usr/bin/env python3
"""Check that a two-gap raw-IQ replay resumes the receive and DL chains."""
import argparse
import json
import re
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", type=Path)
    ap.add_argument("--max-resync-ms", type=int, default=2000)
    args = ap.parse_args()
    lines = [re.sub(r"\x1b\[[0-9;]*m", "", s) for s in args.log.read_text(errors="replace").splitlines()]
    gaps = [i for i, s in enumerate(lines) if "RAW_IQ_GAP injected" in s]
    assert len(gaps) == 2, f"expected two injected gaps, found {len(gaps)}"
    assert not any("RFSTALL" in s for s in lines), "RFSTALL after a replay gap"
    recoveries = []
    metrics = []
    for i, s in enumerate(lines):
        match = re.search(r"RX_RECOVER epoch=(\d+) gap_samples=(-?\d+) resync_ms=(\d+)", s)
        if match:
            recoveries.append((i, *map(int, match.groups())))
        if "ISAC_METRICS {" in s:
            metrics.append((i, json.loads(s.split("ISAC_METRICS ", 1)[1])))
    assert len(recoveries) == 2, f"expected two completed recoveries, found {len(recoveries)}"
    for n, gap_line in enumerate(gaps):
        end = gaps[n + 1] if n == 0 else len(lines)
        before = [m for i, m in metrics if i < gap_line]
        after = [m for i, m in metrics if recoveries[n][0] < i < end]
        assert before and after, f"missing metric window around gap {n + 1}"
        before, after = before[-1], after[-1]
        _, epoch, samples, elapsed = recoveries[n]
        assert epoch == n + 1 and samples > 0 and elapsed <= args.max_resync_ms, (epoch, samples, elapsed)
        assert after["acq_state"] == "CORESET_VERIFIED", f"CORESET not reacquired after gap {n + 1}"
        assert after["csirs_confirmed"] >= max(1, before["csirs_confirmed"]), f"CSI-RS lost after gap {n + 1}"
        assert after["pdcch_accepts"] > before["pdcch_accepts"], f"PDCCH stopped after gap {n + 1}"
        assert after["pdschq_crc_ok"] > before["pdschq_crc_ok"], f"PDSCH stopped after gap {n + 1}"
        print(f"gap {n + 1}: samples={samples} resync_ms={elapsed} "
              f"CSI-RS={before['csirs_confirmed']}->{after['csirs_confirmed']} "
              f"PDCCH={before['pdcch_accepts']}->{after['pdcch_accepts']} "
              f"PDSCH_CRC={before['pdschq_crc_ok']}->{after['pdschq_crc_ok']}")
    assert any("RAW_IQ" in s and "EOF" in s for s in lines), "replay did not reach EOF"


if __name__ == "__main__":
    main()
