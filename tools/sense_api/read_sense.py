#!/usr/bin/env python3
"""Read schema-2 JSONL records and little-endian NRCF v1 CFR frames."""

import argparse
from collections import Counter
from contextlib import nullcontext
import json
import struct
import zipfile

SOURCES = ("ssb", "csirs", "pdsch_dmrs", "pdsch_data", "pusch_dmrs",
           "pusch_data", "prach", "srs")
HEADER = struct.Struct("<4sHHQBBHIQffII")


def read_records(path):
    with open(path, encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            record = json.loads(line)
            if not isinstance(record, dict) or record.get("schema") != 2:
                raise ValueError(f"{path}:{line_number}: expected schema-2 object")
            yield record


def _exact(stream, count):
    data = stream.read(count)
    if len(data) != count:
        raise ValueError("truncated CFR frame")
    return data


def read_frames(path):
    with open(path, "rb") as stream:
        while header := stream.read(HEADER.size):
            if len(header) != HEADER.size:
                raise ValueError("truncated CFR header")
            (magic, version, header_len, frame_id, source, n_ant, reserved, n_re,
             abs_slot, slot_frac, noise_var, epoch, identity_gen) = HEADER.unpack(header)
            if magic != b"NRCF" or version != 1 or header_len < HEADER.size:
                raise ValueError("invalid CFR v1 header")
            if source >= len(SOURCES) or n_ant < 1 or n_ant > 8 or n_re < 1 or n_re > 46200 or reserved:
                raise ValueError("invalid CFR dimensions or source")
            _exact(stream, header_len - HEADER.size)
            k_abs = struct.unpack(f"<{n_re}H", _exact(stream, 2 * n_re))
            l_sym = tuple(_exact(stream, n_re))
            _exact(stream, -(header_len + 3 * n_re) % 4)
            values = struct.unpack(f"<{2 * n_ant * n_re}f", _exact(stream, 8 * n_ant * n_re))
            iq = tuple(tuple(complex(values[2 * (a * n_re + r)],
                                     values[2 * (a * n_re + r) + 1]) for r in range(n_re))
                       for a in range(n_ant))
            yield dict(frame_id=frame_id, source=SOURCES[source], n_ant=n_ant, n_re=n_re,
                       abs_slot=abs_slot, slot_frac=slot_frac, noise_var=noise_var,
                       epoch=epoch, identity_gen=identity_gen, k_abs=k_abs, l_sym=l_sym,
                       iq=iq, iq_shape=(n_ant, n_re))


def load(records_path, cfr_path):
    records = list(read_records(records_path))
    frames = list(read_frames(cfr_path))
    by_id = {frame["frame_id"]: frame for frame in frames}
    if len(by_id) != len(frames):
        raise ValueError("duplicate CFR frame_id")
    previous = 0
    gaps = []
    for frame in frames:
        current = frame["frame_id"]
        if current <= previous:
            raise ValueError("CFR frame_id is not increasing")
        if current > previous + 1:
            gaps.append((previous + 1, current - 1))
        previous = current
    missing = []
    for record in records:
        frame_id = record.get("cfr_frame_id") if record.get("type") == "rs" else None
        record["cfr_frame"] = by_id.get(frame_id) if frame_id is not None else None
        if frame_id is not None and frame_id not in by_id:
            missing.append(frame_id)
        if record.get("type") == "rs" and record.get("k_abs_convention") not in (None, "crb0_point_a"):
            raise ValueError("unknown k_abs convention")
    return dict(records=records, frames=frames, frame_id_gaps=gaps,
                frame_id_drops=sum(end - start + 1 for start, end in gaps),
                missing_links=missing,
                record_sources=Counter(r.get("source") for r in records if r.get("type") == "rs"),
                frame_sources=Counter(f["source"] for f in frames))


def scan(records_path, cfr_path, npz_path=None):
    """Summarize long captures without retaining CFR IQ or records in memory."""
    record_types, record_sources, links = Counter(), Counter(), Counter()
    for record in read_records(records_path):
        record_types[record.get("type")] += 1
        if record.get("type") == "rs":
            record_sources[record.get("source")] += 1
            if record.get("k_abs_convention") not in (None, "crb0_point_a"):
                raise ValueError("unknown k_abs convention")
            if record.get("cfr_frame_id") is not None:
                links[record["cfr_frame_id"]] += 1
    if npz_path:
        try:
            import numpy as np
        except ImportError as error:
            raise RuntimeError("--npz requires numpy") from error
    frame_sources, iq_shapes, gaps, count, previous = Counter(), Counter(), [], 0, 0
    archive = zipfile.ZipFile(npz_path, "w", compression=zipfile.ZIP_STORED,
                              allowZip64=True) if npz_path else nullcontext()
    with archive as output:
        for frame in read_frames(cfr_path):
            frame_id = frame["frame_id"]
            if frame_id <= previous:
                raise ValueError("CFR frame_id is not increasing")
            if frame_id > previous + 1:
                gaps.append((previous + 1, frame_id - 1))
            previous = frame_id
            count += 1
            frame_sources[frame["source"]] += 1
            iq_shapes[frame["iq_shape"]] += 1
            links.pop(frame_id, None)
            if npz_path:
                prefix = f"frame_{frame_id}"
                for suffix, values, dtype in (("iq", frame["iq"], np.complex64),
                                              ("k_abs", frame["k_abs"], np.uint16),
                                              ("l_sym", frame["l_sym"], np.uint8)):
                    with output.open(prefix + "_" + suffix + ".npy", "w", force_zip64=True) as entry:
                        np.lib.format.write_array(entry, np.asarray(values, dtype=dtype),
                                                  allow_pickle=False)
    return dict(record_types=record_types, record_sources=record_sources,
                frame_sources=frame_sources, iq_shapes=iq_shapes, frame_count=count, frame_id_gaps=gaps,
                frame_id_drops=sum(end - start + 1 for start, end in gaps),
                missing_links=list(links.elements()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("records", help="schema-2 JSONL path")
    parser.add_argument("cfr", help="NRCF v1 path")
    parser.add_argument("--npz", metavar="OUT", help="export per-frame IQ, k_abs and l_sym arrays (requires numpy)")
    args = parser.parse_args(argv)
    result = scan(args.records, args.cfr, args.npz)
    print(f"records={sum(result['record_types'].values())} frames={result['frame_count']} "
          f"frame_id_drops={result['frame_id_drops']} missing_links={len(result['missing_links'])}")
    print("record sources:", dict(result["record_sources"]))
    print("CFR sources:", dict(result["frame_sources"]))
    print("k_abs=CRB0/Point A absolute subcarrier; IQ=(n_ant,n_re) complex64, antenna major")
    print("IQ shapes:", dict(result["iq_shapes"]))
    if result["frame_id_gaps"]:
        print("frame_id gaps:", result["frame_id_gaps"])
    return 1 if result["missing_links"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
