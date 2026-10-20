"""Atomic, rotating dataset writer. Every row passes policy.validate_row first (fail closed)."""
import json
import os
import threading

import policy

try:  # optional: JSONL fallback keeps the exporter dependency-free
    import pyarrow as pa
    import pyarrow.parquet as pq
except Exception:  # noqa: BLE001
    pa = pq = None

_PA = {"int": "int64", "float": "float64", "bool": "bool_"}


def _arrow_type(kind):
    return pa.string() if kind.startswith("enum:") else getattr(pa, _PA[kind])()


class Sink:
    def __init__(self, outdir, mode, fmt="auto", rotate_rows=50000):
        if mode not in policy.MODES:
            raise policy.PolicyViolation("unknown mode")
        self.out, self.mode, self.rotate = outdir, mode, rotate_rows
        self.format = "parquet" if (fmt in ("auto", "parquet") and pq is not None) else "jsonl"
        if fmt == "parquet" and pq is None:
            raise RuntimeError("pyarrow not importable")
        self.buf, self.parts = {}, {}
        self.rows_written = self.refused_rows = self.refused_fields = 0
        self.lock = threading.Lock()
        os.makedirs(os.path.join(outdir, ".tmp"), exist_ok=True)

    def write(self, dataset, row):
        try:
            policy.validate_row(self.mode, dataset, row)
        except policy.PolicyViolation as e:
            with self.lock:
                self.refused_rows += 1
                self.refused_fields += e.n_fields
            raise
        with self.lock:
            b = self.buf.setdefault(dataset, [])
            b.append(row)
            if len(b) >= self.rotate:
                self._flush(dataset)

    def flush(self):
        with self.lock:
            for d in list(self.buf):
                self._flush(d)

    def _flush(self, dataset):
        rows = self.buf.pop(dataset, [])
        if not rows:
            return
        n = self.parts.get(dataset, 0)
        self.parts[dataset] = n + 1
        d = os.path.join(self.out, dataset)
        os.makedirs(d, exist_ok=True)
        ext = "parquet" if self.format == "parquet" else "jsonl"
        final = os.path.join(d, "part-%05d.%s" % (n, ext))
        tmp = os.path.join(self.out, ".tmp", "%s-%05d.%s" % (dataset, n, ext))  # same filesystem as final, never /tmp
        if self.format == "parquet":
            cols = sorted({c for r in rows for c in r})
            arrays = [pa.array([r.get(c) for r in rows], type=_arrow_type(policy.column_spec(dataset, c)[0])) for c in cols]
            pq.write_table(pa.table(arrays, names=cols), tmp)
        else:
            with open(tmp, "w") as f:
                for r in rows:
                    f.write(json.dumps(r, separators=(",", ":")) + "\n")
        os.replace(tmp, final)
        self.rows_written += len(rows)

    def write_json(self, name, obj):
        if name not in ("summary.json", "manifest.json"):
            raise policy.PolicyViolation("unknown file")
        tmp = os.path.join(self.out, ".tmp", name)
        with open(tmp, "w") as f:
            json.dump(obj, f, indent=1, sort_keys=True)
        os.replace(tmp, os.path.join(self.out, name))

    def close(self):
        self.flush()
        try:
            os.rmdir(os.path.join(self.out, ".tmp"))
        except OSError:
            pass


def read_dataset(outdir, dataset):
    d = os.path.join(outdir, dataset)
    rows = []
    if not os.path.isdir(d):
        return rows
    for fn in sorted(os.listdir(d)):
        p = os.path.join(d, fn)
        if fn.endswith(".parquet"):
            rows.extend(pq.read_table(p).to_pylist())
        elif fn.endswith(".jsonl"):
            with open(p) as f:
                rows.extend(json.loads(x) for x in f if x.strip())
    return rows
