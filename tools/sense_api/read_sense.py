#!/usr/bin/env python3
"""Read schema-2 JSONL records and little-endian NRCF v1 CFR frames."""

import argparse
from collections import Counter
from contextlib import nullcontext
import cmath
import json
import math
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


def rx_state_report(records):
    """Receiver state the records carry (rx_cfo_applied_hz, rx_shift_cum_samples, rx_shift_count, rx_gain_db, rx_ts), per source, plus the
    window-shift events: where rx_shift_count rises between two consecutive rs records in abs_slot order (all sources merged).
    rx_shift_cum_samples > 0 = the read window moved later in the stream; a step of d samples multiplies the CFR of subcarrier k by
    exp(+2j*pi*k*d/N) from then on, so coherent processing across a step needs that correction (see coherence_check)."""
    per = {}
    rs = [r for r in records if r.get("type") == "rs"]
    for r in rs:
        e = per.setdefault(r.get("source"), dict(n=0, with_state=0, with_rx_ts=0, cfo_hz=[None, None], gain_db=set()))
        e["n"] += 1
        if r.get("rx_shift_count") is not None:
            e["with_state"] += 1
            cfo = r.get("rx_cfo_applied_hz")
            if cfo is not None:
                e["cfo_hz"] = [cfo if e["cfo_hz"][0] is None else min(e["cfo_hz"][0], cfo),
                               cfo if e["cfo_hz"][1] is None else max(e["cfo_hz"][1], cfo)]
            if r.get("rx_gain_db") is not None:
                e["gain_db"].add(r["rx_gain_db"])
        if r.get("rx_ts") is not None:
            e["with_rx_ts"] += 1
    for e in per.values():
        e["gain_db"] = sorted(e["gain_db"])
    events, prev = [], None
    for r in sorted((r for r in rs if r.get("rx_shift_count") is not None and r.get("abs_slot") is not None),
                    key=lambda r: r["abs_slot"]):
        if prev is not None and r["rx_shift_count"] > prev["rx_shift_count"]:
            events.append(dict(abs_slot_before=prev["abs_slot"], abs_slot_after=r["abs_slot"], steps=r["rx_shift_count"] - prev["rx_shift_count"],
                               shift_samples=r["rx_shift_cum_samples"] - prev["rx_shift_cum_samples"]))
        prev = r
    return dict(per_source=per, window_shifts=events)


def session_fft_size(records):
    """FFT size N = fs_hz / scs of the first session record that has both (None when absent)."""
    for r in records:
        if r.get("type") == "session" and r.get("fs_hz") and r.get("scs_khz"):
            return round(r["fs_hz"] / (r["scs_khz"] * 1000))
    return None


def _slope_phase(h1, h2):
    """-> (slope rad/subcarrier, mean phase rad, n_pairs) of h2/h1 over the subcarriers both have (dicts k -> complex)."""
    common = sorted(set(h1) & set(h2))
    if len(common) < 2:
        return None
    gaps = Counter(b - a for a, b in zip(common, common[1:]))
    d = min(gaps)
    acc, pairs = 0j, 0
    for a, b in zip(common, common[1:]):
        if b - a == d:
            acc += (h2[b] * h2[a].conjugate()) * (h1[b] * h1[a].conjugate()).conjugate()
            pairs += 1
    phi0 = cmath.phase(sum(h2[k] * h1[k].conjugate() for k in common))
    return (cmath.phase(acc) / d if pairs else None), phi0, pairs


def coherence_check(records_path, cfr_path, n_fft=None, tol=0.01, max_dslot=None):
    """Phase continuity of the CFR across slots, flagged at window shifts.
    For each consecutive pair of CFR frames of one stream (source, rnti, start_rb, csirs_resource) the phase slope of H2/H1 across subcarriers
    (rad/subcarrier, antenna 0, averaged over the symbols) is measured. A window step of d samples between the two frames predicts a slope of 2*pi*d/N
    (d = difference of rx_shift_cum_samples, N = FFT size from the session record or `n_fft`); anything else is timing drift or a break.
    Each pair is returned with flag "ok" (|residual| <= tol), "window_shift" (d != 0, residual within tol of the prediction) or "break"
    (residual above tol: the channel changed or the receiver moved the window without recording it). Pairs whose records lack receiver state get "no_state".
    Returns dict(n_fft, pairs=[...], counts=Counter of flags)."""
    records = list(read_records(records_path))
    n = n_fft or session_fft_size(records)
    by_frame = {r["cfr_frame_id"]: r for r in records if r.get("type") == "rs" and r.get("cfr_frame_id") is not None}
    last, pairs = {}, []
    for frame in read_frames(cfr_path):
        rec = by_frame.get(frame["frame_id"])
        if rec is None:
            continue
        h = {}
        for r in range(frame["n_re"]):
            h.setdefault(frame["k_abs"][r], []).append(frame["iq"][0][r])
        h = {k: sum(v) / len(v) for k, v in h.items()}
        key = (rec["source"], rec.get("rnti"), rec.get("start_rb"), rec.get("csirs_resource"))
        if key in last:
            prev_rec, prev_h = last[key]
            if max_dslot is None or rec["abs_slot"] - prev_rec["abs_slot"] <= max_dslot:
                sp = _slope_phase(prev_h, h)
                if sp and sp[0] is not None:
                    have = rec.get("rx_shift_cum_samples") is not None and prev_rec.get("rx_shift_cum_samples") is not None
                    d = rec["rx_shift_cum_samples"] - prev_rec["rx_shift_cum_samples"] if have else None
                    exp = 2 * math.pi * d / n if have and n else 0.0
                    resid = (sp[0] - exp + math.pi) % (2 * math.pi) - math.pi
                    flag = "no_state" if not have else ("break" if abs(resid) > tol else ("window_shift" if d else "ok"))
                    pairs.append(dict(source=key[0], rnti=key[1], abs_slot_before=prev_rec["abs_slot"], abs_slot_after=rec["abs_slot"],
                                      shift_samples=d, slope=sp[0], expected_slope=exp, residual_slope=resid, mean_phase=sp[1], flag=flag))
        last[key] = (rec, h)
    return dict(n_fft=n, pairs=pairs, counts=Counter(p["flag"] for p in pairs))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("records", help="schema-2 JSONL path")
    parser.add_argument("cfr", help="NRCF v1 path")
    parser.add_argument("--coherence", action="store_true", help="phase continuity across slots, flagged at window shifts")
    parser.add_argument("--n-fft", type=int, help="FFT size for --coherence when the capture has no session record")
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
    records = list(read_records(args.records))
    rx = rx_state_report(records)
    for source, e in sorted(rx["per_source"].items()):
        print(f"rx state {source}: records={e['n']} with_state={e['with_state']} with_rx_ts={e['with_rx_ts']} "
              f"cfo_hz={e['cfo_hz']} gain_db={e['gain_db']}")
    print(f"window shifts: {len(rx['window_shifts'])}", rx["window_shifts"][:10])
    if args.coherence:
        co = coherence_check(args.records, args.cfr, args.n_fft)
        print(f"coherence (n_fft={co['n_fft']}): pairs={len(co['pairs'])}", dict(co["counts"]))
        for p in [p for p in co["pairs"] if p["flag"] in ("break", "window_shift")][:20]:
            print("  ", p["flag"], p["source"], p["rnti"], p["abs_slot_before"], "->", p["abs_slot_after"],
                  "shift", p["shift_samples"], "slope %.4f expected %.4f" % (p["slope"], p["expected_slope"]))
    return 1 if result["missing_links"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
