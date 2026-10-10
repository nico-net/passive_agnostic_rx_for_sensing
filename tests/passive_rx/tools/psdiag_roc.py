#!/usr/bin/env python3
"""ACQ-NIGHT: ROC of the PDCCH prescreen statistics from PSDIAG lines of a replay run.log.
usage: psdiag_roc.py run.log [run.log ...]   (histograms of several runs are summed)
Positives = candidates that passed CRC with a known RNTI ("ok"); negatives = all other candidates ("all" minus "ok").
Threshold z means score >= 2/pi + z*0.339/sqrt(E), E = 108*L soft-bit values."""
import re, sys, math, collections
pat = re.compile(r'PSDIAG (\w+) tok=(\d+) nid=(-?\d+) rbo=(-?\d+) kind=(\w+) AL=(\d+) metric=(\d) lo=(\S+) step=(\S+) n=(\d+) bins=(\S*)')
def load(paths, ctx):
    H = collections.defaultdict(lambda: collections.defaultdict(int))
    for p in paths:
        best = {}  # cumulative dumps: keep the one with the largest n per (tok, kind, AL, metric)

        for line in open(p, errors='replace'):
            m = pat.search(line)
            if not m or m.group(1) != ctx: continue
            _, tok, nid, rbo, kind, al, met, lo, step, n, bins = m.groups()
            key = (tok, kind, int(al), int(met))
            if key in best and best[key][0] >= int(n): continue
            best[key] = (int(n), (kind, int(al), int(met), float(lo), float(step)), bins)
        for n, hk, bins in best.values():
            for kv in bins.split(','):
                if kv.count(':') == 1:
                    b, c = kv.split(':'); H[hk][int(b)] += int(c)
    return H
def main():
    ctx = 'bank'
    args = [a for a in sys.argv[1:] if not a.startswith('--ctx=')]
    for a in sys.argv[1:]:
        if a.startswith('--ctx='): ctx = a[6:]
    H = load(args, ctx)
    als = sorted({k[1] for k in H})
    for al in als:
        E = 108 * al
        sig = 0.339 / math.sqrt(E); mu = 2 / math.pi
        for met, name in ((0, 'score'),):
            allk = [k for k in H if k[0] == 'all' and k[1] == al and k[2] == met]
            okk = [k for k in H if k[0] == 'ok' and k[1] == al and k[2] == met]
            if not allk: continue
            ka = allk[0]; lo, step = ka[3], ka[4]
            ha = H[ka]; ho = H[okk[0]] if okk else {}
            Na, No = sum(ha.values()), sum(ho.values()); Nn = Na - No
            print(f'AL{al}: candidates={Na} ok(real DCI)={No} negatives={Nn}  noise mean {mu:.3f} sigma {sig:.4f}')
            if No == 0: continue
            print('   z    thr   miss(ok<thr)  pass(neg>=thr)  pass(neg)/cand  neg_reject')
            for z in (0, 1, 2, 3.09, 4, 4.5, 5, 6, 7, 8):
                thr = mu + z * sig
                b = int(math.floor((thr - lo) / step))
                miss = sum(c for k, c in ho.items() if k < b)
                passn = sum(c - ho.get(k, 0) for k, c in ha.items() if k >= b)
                print(f'  {z:5.2f} {thr:.3f}  {miss:6d} ({miss/No*100:6.3f}%)  {passn:8d}  {passn/max(Nn,1)*100:7.3f}%  {100-passn/max(Nn,1)*100:7.3f}%')
            # lowest ok scores
            low = sorted(ho.items())[:6]
            print('   lowest ok score bins:', [(round(lo + k * step, 3), c) for k, c in low])
if __name__ == "__main__": main()

def generic(paths, ctx, targets=(0.0, 0.001, 0.01, 0.05)):
    """For every metric and AL: the threshold achieving a given miss rate on real DCIs and the resulting negative reject rate."""
    H = load(paths, ctx)
    names = {0: 'score', 1: 'energy/floor', 2: 'dmrs corr'}
    print('--- generic ROC (threshold = lowest value keeping miss <= target) ---')
    for al in sorted({k[1] for k in H}):
        for met in (0, 1, 2):
            ka = [k for k in H if k[0] == 'all' and k[1] == al and k[2] == met]
            ko = [k for k in H if k[0] == 'ok' and k[1] == al and k[2] == met]
            if not ka or not ko: continue
            lo, step = ka[0][3], ka[0][4]
            ha, ho = H[ka[0]], H[ko[0]]
            No = sum(ho.values()); Nn = sum(ha.values()) - No
            for t in targets:
                allowed = int(t * No); cum = 0; b = 0
                for k in sorted(ho):
                    if cum + ho[k] > allowed: b = k; break
                    cum += ho[k]; b = k + 1
                rej = sum(c - ho.get(k, 0) for k, c in ha.items() if k < b)
                print(f'AL{al} {names[met]:13s} miss<={t*100:5.2f}% thr={lo + b*step:8.3f}  neg_reject={rej/max(Nn,1)*100:7.3f}%  (ok={No} neg={Nn})')
if __name__ == '__main__':
    pass
