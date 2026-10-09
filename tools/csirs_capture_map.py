#!/usr/bin/env python3
"""CSI-FULL: offline characterisation of the CSI-RS / TRS / ZP-CSI-RS the gNB transmits, from a raw 4-RX capture.

Ground-truth-free: nothing here reads gNB logs/configs. Reads raw_iq sc16 files (rx0..rx3.sc16, 122.88 Msps,
217 PRB mu=1, Point A = grid start at the capture centre), so it is a heavy job: run it through heavy.sh.

Steps
  1. Timing: cyclic-prefix correlation folded over slots (slot start incl. long CP), CFO from the CP phase.
  2. Frame alignment: TS 38.211 7.4.1.5.2 CSI-RS sequence of the serving cell's TRS (row 1, fd 0x1, l=4,
     RB 0..51, scramblingID = --align-id) searched over 160 slot phases x 20 slot numbers -> absolute slot % 160.
  3. Map pass ("map"): per slot%160, symbol, RB, subcarrier class (k mod 12):
       - coherence: |sum_ant Y_p conj Y_{p+1}| / sqrt(P_p P_{p+1}) between consecutive 160-slot periods --
         ID-agnostic detector of anything that repeats (CSI-RS, TRS, DM-RS of repeated grants, SSB);
       - mean power per RE (ZP-CSI-RS = REs that stay empty while their RB neighbours carry PDSCH).
  4. ID pass ("ids"): for chosen slot phases, every symbol, k0 in 0..3 and all 1024 scrambling IDs, the per-RB
     row-1 (density 3) matched-filter ratio |sum_i Y r*|^2 / (3 sum_i |Y|^2), averaged over periods (1 = noise,
     ~3 = the resource is present in that RB): gives per-occasion RB extent and ID.
Outputs a .npz plus a text summary on stdout.
"""
import argparse, json, os, sys
import numpy as np

FS = 122880000
NFFT = 4096
CP = 288
CP0 = 352
SLOT = 61440
NRB = 217
NSC = NRB * 12
FIRST = NFFT - NSC // 2  # OAI first_carrier_offset: subcarrier k -> bin (FIRST + k) % NFFT
SYM_START = [0] + [CP0 + NFFT + (l - 1) * (CP + NFFT) for l in range(1, 14)]
CP_LEN = [CP0] + [CP] * 13
BACKOFF = 72  # samples of CP left before the FFT window (delay-spread margin); phase compensated below


class Cap:
    def __init__(self, d, nant):
        self.ch = [np.memmap(os.path.join(d, f"rx{a}.sc16"), dtype=np.int16, mode="r") for a in range(nant)]
        self.n = self.ch[0].size // 2

    def read(self, start, count):
        out = np.empty((len(self.ch), count), np.complex64)
        for a, m in enumerate(self.ch):
            s = np.asarray(m[2 * start:2 * (start + count)], dtype=np.float32)
            out[a] = s[0::2] + 1j * s[1::2]
        return out


def timing(cap, start, nslots=40):
    x = cap.read(start, (nslots + 1) * SLOT)
    p = (x[:, :-NFFT] * np.conj(x[:, NFFT:])).sum(0)
    cs = np.concatenate([[0], np.cumsum(p)])
    w = cs[CP:] - cs[:-CP]  # window sum of CP length 288 starting at t
    L = nslots * SLOT
    fold = w[:L].reshape(nslots, SLOT).sum(0)
    fold_ext = np.concatenate([fold, fold])
    # score each tau as sum over the 14 symbols (long CP: use its last 288 samples)
    score = np.zeros(SLOT)
    for l in range(14):
        off = SYM_START[l] + (CP_LEN[l] - CP)
        score += np.abs(fold_ext[off:off + SLOT])
    tau = int(np.argmax(score))
    ph = np.angle(fold_ext[tau + SYM_START[3]])
    cfo = -ph * FS / (2 * np.pi * NFFT)
    return (start + tau) % SLOT, cfo, float(score[tau] / np.median(score))


def gold(cinit, n):
    """TS 38.211 5.2.1, vectorised over an array of c_init."""
    cinit = np.atleast_1d(np.asarray(cinit, dtype=np.int64))
    Nc = 1600
    tot = n + Nc
    x1 = np.zeros(tot + 31, np.uint8)
    x1[0] = 1
    for i in range(tot):
        x1[i + 31] = x1[i + 3] ^ x1[i]
    x2 = np.zeros((cinit.size, tot + 31), np.uint8)
    for b in range(31):
        x2[:, b] = (cinit >> b) & 1
    for i in range(tot):
        x2[:, i + 31] = x2[:, i + 3] ^ x2[:, i + 2] ^ x2[:, i + 1] ^ x2[:, i]
    return (x1[Nc:Nc + n][None, :] ^ x2[:, Nc:Nc + n])


def csirs_seq(ids, ns, l, nre):
    """r(m) for m in [0, nre) -- TS 38.211 7.4.1.5.2."""
    ids = np.atleast_1d(np.asarray(ids, dtype=np.int64))
    cinit = ((1 << 10) * (14 * ns + l + 1) * (2 * ids + 1) + ids) % (1 << 31)
    c = gold(cinit, 2 * nre).astype(np.float32)
    return ((1 - 2 * c[:, 0::2]) + 1j * (1 - 2 * c[:, 1::2])) / np.sqrt(2)


class Slotter:
    def __init__(self, cap, t0, cfo, drift):
        self.cap, self.t0, self.cfo, self.drift = cap, t0, cfo, drift
        k = np.arange(NSC)
        self.bins = (FIRST + k) % NFFT
        self.bo = np.exp(2j * np.pi * k * BACKOFF / NFFT).astype(np.complex64)

    def slot(self, j, syms=range(14)):
        s0 = self.t0 + j * SLOT + int(round(self.drift * j))
        if s0 < 0 or s0 + SLOT + 64 > self.cap.n:
            return None
        x = self.cap.read(s0, SLOT)
        t = (s0 + np.arange(SLOT)) / FS
        x *= np.exp(-2j * np.pi * self.cfo * t).astype(np.complex64)[None, :]
        syms = list(syms)
        Y = np.empty((x.shape[0], len(syms), NSC), np.complex64)
        for i, l in enumerate(syms):
            a = SYM_START[l] + CP_LEN[l] - BACKOFF
            F = np.fft.fft(x[:, a:a + NFFT], axis=1)
            Y[:, i] = F[:, self.bins] * self.bo[None, :]
        return Y


def row1_ratio(Y, refs, k0, rb0=0, nrb=NRB):
    """Y: (ant, NSC) one symbol. refs: (nid, 3*NRB). -> num (nid, nrb), den (nrb,)."""
    ks = (np.arange(rb0, rb0 + nrb)[:, None] * 12 + k0 + 4 * np.arange(3)[None, :])  # (nrb,3)
    y = Y[:, ks]  # (ant, nrb, 3)
    r = refs[:, 3 * rb0:3 * (rb0 + nrb)].reshape(refs.shape[0], nrb, 3)
    # batched over RB: (nrb, ant, 3) @ (nrb, 3, nid) -> (nrb, ant, nid)
    c = np.matmul(np.transpose(y, (1, 0, 2)), np.conj(np.transpose(r, (1, 2, 0))))
    num = (np.abs(c) ** 2).sum(1).T  # (nid, nrb)
    den = (np.abs(y) ** 2).sum((0, 2))  # noise-only: num/den ~ 1; present: up to 3
    return num, den


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cap")
    ap.add_argument("--out", required=True)
    ap.add_argument("--nant", type=int, default=4)
    ap.add_argument("--align-id", type=int, default=415)
    ap.add_argument("--mode", choices=["map", "ids"], default="map")
    ap.add_argument("--phases", default="15,16,20,21")
    ap.add_argument("--syms", default="")
    ap.add_argument("--max-periods", type=int, default=1000)
    a = ap.parse_args()
    cap = Cap(a.cap, a.nant)
    # 1. timing at the start and end -> linear drift
    tA, cfoA, qA = timing(cap, 0)
    endstart = cap.n - 42 * SLOT
    tB, cfoB, qB = timing(cap, endstart - (endstart % SLOT))
    nsl = (cap.n - 2 * SLOT) // SLOT
    d = ((tB - tA + SLOT // 2) % SLOT) - SLOT // 2
    drift = d / nsl
    cfo = 0.5 * (cfoA + cfoB)
    print(f"timing t0={tA} q={qA:.1f}  end={tB} q={qB:.1f}  drift={drift*1e3:.4f} samples/kslot  cfo={cfoA:.0f}/{cfoB:.0f} Hz", flush=True)
    # 2. alignment from the serving TRS (row1 fd1 l4 RB 0..51); the CP fold can slip by whole symbols, so the
    #    slot start is also searched over +-13 symbols (the TRS sequence fixes symbol AND slot number)
    refs = np.concatenate([csirs_seq(a.align_id, ns, 4, 3 * NRB) for ns in range(20)])  # (20, 651)
    best = (-1, 0, 0, tA)
    for sh in range(-13, 14):
        t0 = (tA + sh * (CP + NFFT)) % SLOT
        slx = Slotter(cap, t0, cfo, drift)
        for j in range(160 + 160):
            Y = slx.slot(j, [4])
            num, den = row1_ratio(Y[:, 0], refs, 0, 0, 52)
            sc = num.sum(1) / den.sum()
            ns = int(np.argmax(sc))
            if sc[ns] > best[0]:
                best = (float(sc[ns]), j, ns, t0)
    sc, jbest, nsbest, tA = best
    sl = Slotter(cap, tA, cfo, drift)
    nslots = int((cap.n - tA - 2 * SLOT) // SLOT)
    # abs_slot % 160 == 15 at the TRS occasion, and its slot-in-frame must be 15 (n_s from the sequence)
    print(f"align: slot j={jbest} n_s={nsbest} t0={tA} ratio={sc:.2f} (noise ~1)", flush=True)
    phase_of = lambda j: (15 + (j - jbest)) % 160 if nsbest == 15 else (nsbest + (j - jbest)) % 160
    if nsbest not in (15, 16):
        print("WARNING: TRS n_s is not 15/16; phase assumed = n_s (+ frame%8 unknown)")
    if nsbest == 16:
        phase_of = lambda j: (16 + (j - jbest)) % 160
    j0 = (jbest - phase_of(jbest)) % 160  # first j with phase 0
    nper = min(a.max_periods, (nslots - j0) // 160)
    meta = dict(t0=int(tA), cfo=cfo, drift=drift, jbest=jbest, ns=nsbest, ratio=sc, j0=j0, periods=nper, cap=a.cap)
    print(json.dumps(meta), flush=True)
    if a.mode == "map":
        P = np.zeros((160, 14, NSC), np.float64)
        C = np.zeros((160, 14, NSC), np.float64)
        N = np.zeros((160, 14, NSC), np.float64)
        prev = [None] * 160
        for p in range(nper):
            for ph in range(160):
                Y = sl.slot(j0 + p * 160 + ph)
                if Y is None:
                    continue
                pw = (np.abs(Y) ** 2).sum(0)
                P[ph] += pw
                if prev[ph] is not None:
                    Yp, pwp = prev[ph]
                    C[ph] += np.abs((Y * np.conj(Yp)).sum(0))
                    N[ph] += np.sqrt(pw * pwp)
                prev[ph] = (Y, pw)
            if p % 10 == 0:
                print(f"period {p}/{nper}", flush=True)
        np.savez_compressed(a.out, P=P / max(nper, 1), C=C, N=N, meta=json.dumps(meta))
    else:
        phases = [int(x) for x in a.phases.split(",")]
        syms = [int(x) for x in a.syms.split(",")] if a.syms else list(range(14))
        ids = np.arange(1024)
        NUM = np.zeros((len(phases), len(syms), 4, 1024, NRB), np.float32)
        DEN = np.zeros((len(phases), len(syms), 4, NRB), np.float32)
        refcache = {}
        for p in range(nper):
            for pi, ph in enumerate(phases):
                Y = sl.slot(j0 + p * 160 + ph, syms)
                if Y is None:
                    continue
                ns = ph % 20
                for si, l in enumerate(syms):
                    key = (ns, l)
                    if key not in refcache:
                        refcache[key] = csirs_seq(ids, ns, l, 3 * NRB).astype(np.complex64)
                    for k0 in range(4):
                        num, den = row1_ratio(Y[:, si], refcache[key], k0)
                        # per-period normalisation: noise-only gives num/den ~ 1 per RB
                        NUM[pi, si, k0] += num / np.maximum(den, 1e-9)[None, :]
                        DEN[pi, si, k0] += 1
            print(f"period {p}/{nper}", flush=True)
        np.savez_compressed(a.out, R=NUM / np.maximum(DEN[:, :, :, None, :], 1), phases=phases, syms=syms,
                            meta=json.dumps(meta))


if __name__ == "__main__":
    main()
