"""Log-linear mergeable histogram over non-negative integer microseconds.

Values < 32 are exact; above, every power of two is split in 32 sub-buckets, so the relative bucket width
is <= 1/32 (3.1 %). Sparse (dict of bucket -> count), merge = add counts, so windows can be rolled up exactly.
"""
SUB = 32
SUB_BITS = 5
MAX_US = (1 << 40) - 1  # ~12.7 days; larger values clamp


def _index(v):
    if v < SUB:
        return v
    e = v.bit_length() - 1
    return (e - SUB_BITS + 1) * SUB + ((v >> (e - SUB_BITS)) & (SUB - 1))


def bounds(idx):
    """[lo, hi) of bucket idx in us."""
    if idx < SUB:
        return idx, idx + 1
    e = idx // SUB + SUB_BITS - 1
    lo = (SUB + idx % SUB) << (e - SUB_BITS)
    return lo, lo + (1 << (e - SUB_BITS))


class Hist:
    def __init__(self):
        self.b = {}
        self.n = 0
        self.sum = 0
        self.min = None
        self.max = None

    def add(self, v, count=1):
        v = min(max(int(v), 0), MAX_US)
        i = _index(v)
        self.b[i] = self.b.get(i, 0) + count
        self.n += count
        self.sum += v * count
        self.min = v if self.min is None or v < self.min else self.min
        self.max = v if self.max is None or v > self.max else self.max

    def merge(self, o):
        for i, c in o.b.items():
            self.b[i] = self.b.get(i, 0) + c
        self.n += o.n
        self.sum += o.sum
        if o.min is not None:
            self.min = o.min if self.min is None else min(self.min, o.min)
            self.max = o.max if self.max is None else max(self.max, o.max)
        return self

    def quantile(self, q):
        """Bucket midpoint at nearest rank; None when empty. Clamped to the exact min/max."""
        if not self.n:
            return None
        rank = max(1, int(q * self.n + 0.999999999))
        acc = 0
        for i in sorted(self.b):
            acc += self.b[i]
            if acc >= rank:
                lo, hi = bounds(i)
                return min(max((lo + hi - 1) / 2.0, self.min), self.max)
        return float(self.max)

    def mean(self):
        return self.sum / self.n if self.n else None

    def to_rows(self):
        """[(lo_us, hi_us, count)] ascending."""
        return [(*bounds(i), self.b[i]) for i in sorted(self.b)]

    @classmethod
    def from_rows(cls, rows):
        h = cls()
        for lo, hi, c in rows:
            i = _index(int(lo))
            h.b[i] = h.b.get(i, 0) + int(c)
            h.n += int(c)
            h.sum += int(c) * int((lo + hi - 1) // 2)  # bucket-mid approximation; exact sum is not stored in rows
            h.min = lo if h.min is None else min(h.min, lo)
            h.max = hi - 1 if h.max is None else max(h.max, hi - 1)
        return h
