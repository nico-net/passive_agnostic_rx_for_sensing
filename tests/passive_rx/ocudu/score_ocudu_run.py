#!/usr/bin/env python3
"""Score one run_ocudu_passive.sh run dir: passive blind-decode result vs OCUDU ground truth.
Usage: score_ocudu_run.py RUN_DIR   (gnb.log is ground truth, used for validation only)"""
import collections, re, statistics, sys

run = sys.argv[1]
px = open(f'{run}/passive.log', errors='replace').read().splitlines()
gnb = open(f'{run}/gnb.log', errors='replace').read().splitlines()

def ts(line):  # OAI wall_clock prefix, if enabled
    m = re.match(r'\s*\[?(\d{9,}\.\d+)', line)
    return float(m.group(1)) if m else None

def last(pat, lines=px):
    for l in reversed(lines):
        if re.search(pat, l):
            return l
    return ''

# ---- ground truth ----
crnti = collections.Counter(re.findall(r'tc-rnti=(0x[0-9a-f]+)', '\n'.join(gnb)))
c = crnti.most_common(1)[0][0] if crnti else None
pd = collections.Counter()
sizes = collections.Counter()
for l in gnb:
    m = re.search(rf'PDSCH: rnti={c} .*?symb=\[(\d+), (\d+)\) mod=(\S+) .*?bg=(\S+)', l) if c else None
    if m:
        s, e = int(m.group(1)), int(m.group(2))
        pd[f'S{s} L{e - s} {m.group(3)} {m.group(4)}'] += 1
    m = re.search(rf'PDCCH: rnti={c} .*?format=(\S+).*?size=(\d+)', l) if c else None
    if m:
        sizes[(m.group(1), int(m.group(2)))] += 1
pu = collections.Counter(re.findall(rf'PUSCH: rnti={c} .*?crc=(OK|KO)', '\n'.join(gnb))) if c else {}
sinr = [float(x) for x in re.findall(rf'PUSCH: rnti={c} .*?sinr=([-0-9.]+)dB', '\n'.join(gnb))] if c else []

print(f'== truth: C-RNTI {c}  PDSCH {pd.most_common(3)}')
print(f'   DCI sizes (needs gNB phy_level debug): {dict(sizes) or "n/a"}')
print(f'   gNB PUSCH OK/KO {dict(pu)}  SINR median {statistics.median(sinr) if sinr else "n/a"} dB')

# ---- passive ----
armed = sum('Technique D ARMED' in l for l in px)
conv = [l for l in px if 'Technique D CONVERGED' in l]
first_c = next((l for l in px if c and re.search(rf'rnti_seen .*rnti={c}\b', l)), None)
t_first = int(re.search(r'utc_ns=(\d+)', first_c).group(1)) / 1e9 if first_c else None
print(f'== passive: Technique D ARMED x{armed}, CONVERGED x{len(conv)}')
for l in conv[:4]:
    t = ts(l)
    dt = f' t+{t - t_first:.1f}s after first C-RNTI accept' if (t and t_first) else ''
    print('  ', re.sub(r'.*SENSING: ', '', l).strip() + dt)
for pat in (r'DCI 1_1 length locked', r'UL automatic DCI length locked', r'CORESET VERIFIED', r'STAGE0 DECIDED'):
    l = next((x for x in px if re.search(pat, x)), '')
    print('  ', re.sub(r'.*(SENSING: |\] )', '', l).strip()[:160] if l else f'{pat}: none')
summ = last('blind PDCCH monitor summary')
for k in ('occasions=[0-9]+', r'dci10\[[^]]*\]', r'dci01\[[^]]*\]', r'pdsch_decode\[[^]]*\]', r'scanq\[[^]]*\]'):
    m = re.search(k, summ)
    print('  ', m.group(0) if m else f'{k}: none')
print('  ', re.sub(r'.*SENSING: ', '', last('PDSCHQ per-rnti')).strip()[:200])
print('  ', re.sub(r'.*SENSING: ', '', last(r'pusch_passive\[')).strip()[:200])
