#!/usr/bin/env python3
"""Summarise uectx/1 JSON Lines without needing receiver or gNB logs."""
import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


def load_records(path):
    records = []
    with Path(path).open(encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("schema") != "uectx/1":
                raise ValueError(f"{path}:{number}: expected uectx/1")
            if record.get("type") not in {"ue_snapshot", "ue_change", "ue_reconfig"}:
                raise ValueError(f"{path}:{number}: unknown record type")
            records.append(record)
    return records


def timeline(records, rnti=None):
    grouped = defaultdict(list)
    for record in records:
        if rnti is None or record["rnti"] == rnti:
            key = (record["identity_gen"], record["rnti"], record["incarnation"])
            grouped[key].append(record)
    return grouped


def csv_rows(records):
    for record in records:
        if record["type"] == "ue_snapshot":
            continue
        yield {
            "identity_gen": record["identity_gen"],
            "rnti": record["rnti"],
            "incarnation": record["incarnation"],
            "t_mono_ns": record["t_mono_ns"],
            "abs_slot": record.get("abs_slot"),
            "type": record["type"],
            "param": record.get("param"),
            "old": record.get("old"),
            "new": record.get("new"),
            "cause": record.get("cause"),
            "evidence": record.get("evidence"),
            "class": record.get("class"),
            "params": ",".join(record.get("params", [])),
        }


def render(records, rnti=None):
    lines = []
    groups = timeline(records, rnti)
    counts = Counter(r["type"] for group in groups.values() for r in group)
    lines.append(f"UE contexts: {len(groups)}; snapshots: {counts['ue_snapshot']}; "
                 f"changes: {counts['ue_change']}; reconfigurations: {counts['ue_reconfig']}")
    for key in sorted(groups):
        lines.append(f"\nidentity={key[0]} rnti=0x{key[1]:04x} incarnation={key[2]}")
        for record in sorted(groups[key], key=lambda r: r["t_mono_ns"]):
            at = f"t={record['t_mono_ns']}"
            if record["type"] == "ue_snapshot":
                known = [f"{p}={v['value']}" for p, v in record["cfg"].items() if v is not None]
                lines.append(f"  {at} {record['state']} " + " ".join(known))
            elif record["type"] == "ue_change":
                detail = ""
                if record["param"] == "APERIODIC_CSI" and record.get("evidence") is not None:
                    detail = f"; coincident CSI-RS resource {record['evidence']}"
                lines.append(f"  {at} change {record['param']}: {record['old']} -> "
                             f"{record['new']} ({record['cause']}{detail})")
            else:
                lines.append(f"  {at} reconfig {record['class']}: "
                             + ", ".join(record["params"]))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--rnti", type=lambda s: int(s, 0))
    parser.add_argument("--csv", type=Path)
    args = parser.parse_args()
    records = load_records(args.file)
    selected = [r for group in timeline(records, args.rnti).values() for r in group]
    print(render(records, args.rnti))
    if args.csv:
        fields = ["identity_gen", "rnti", "incarnation", "t_mono_ns", "abs_slot",
                  "type", "param", "old", "new", "cause", "evidence", "class", "params"]
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(csv_rows(selected))


if __name__ == "__main__":
    main()
