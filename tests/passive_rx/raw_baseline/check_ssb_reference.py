#!/usr/bin/env python3
"""Offline PSS/SSS coverage at an explicit reference frequency, NOT autonomous acquisition.
TS 38.211 7.4.2; no PBCH CRC, SIB1, CSI-RS or DCI acceptance is inferred here.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from scipy import signal

NFFT = 256
CP = 18  # normal CP at the explicitly supported 30-kHz / 7.68-MSps analysis geometry
BINS = np.arange(-64, 63) % NFFT


def sequence(seed, tap):
    x = np.zeros(127, dtype=np.int8)
    x[:7] = seed
    for n in range(120):
        x[n + 7] = x[n + tap] ^ x[n]
    return x


def pss(nid2):
    x = sequence([0, 1, 1, 0, 1, 1, 1], 4)
    return 1 - 2 * x[(np.arange(127) + 43 * nid2) % 127]


def sss_catalog(nid2):
    x0 = sequence([1, 0, 0, 0, 0, 0, 0], 4)
    x1 = sequence([1, 0, 0, 0, 0, 0, 0], 1)
    identities = np.arange(336)[:, None]
    n = np.arange(127)[None, :]
    m0 = 15 * (identities // 112) + 5 * nid2
    m1 = identities % 112
    return (1 - 2 * x0[(n + m0) % 127]) * (1 - 2 * x1[(n + m1) % 127])


def symbol(values):
    grid = np.zeros(NFFT, dtype=np.complex64)
    grid[BINS] = values
    return np.fft.ifft(grid).astype(np.complex64)


def detect(y, rate):
    # Use double precision before FFT correlation and energy accumulation.
    y = np.asarray(y, dtype=np.complex128)
    if rate != 7680000 or len(y) < 4 * (NFFT + CP):
        raise ValueError('unsupported analysis geometry')
    energy = np.r_[0.0, np.cumsum(np.abs(y).astype(np.float64) ** 2)]
    energy = energy[NFFT:] - energy[:-NFFT]
    best = None
    for nid2 in range(3):
        base = symbol(pss(nid2))
        for coarse in (-30000, -15000, 0, 15000, 30000):
            reference = base * np.exp(2j * np.pi * coarse * np.arange(NFFT) / rate)
            corr = signal.correlate(y, reference, mode='valid', method='fft')
            denom = energy * np.vdot(reference, reference).real
            # Empty windows contain no evidence, regardless of FFT roundoff.
            scores = np.divide(np.abs(corr) ** 2, denom,
                               out=np.zeros_like(energy), where=energy > 0)
            score = float(np.max(scores))
            if best is None or score > best[0]:
                best = (score, nid2, coarse, scores)
    peak_score, nid2, coarse, scores = best
    positions, _ = signal.find_peaks(scores, height=0.35, distance=4 * (NFFT + CP))
    results = []
    catalog = sss_catalog(nid2)
    for pos in positions:
        sss_pos = int(pos) + 2 * (NFFT + CP)
        if pos < CP or sss_pos + NFFT > len(y):
            continue
        idx = np.arange(pos - CP, pos + NFFT)
        block = y[pos - CP:pos + NFFT] * np.exp(-2j * np.pi * coarse * idx / rate)
        residual = np.angle(np.vdot(block[:CP], block[NFFT:NFFT + CP])) * rate / (2 * np.pi * NFFT)
        cfo = coarse + float(residual)
        first = y[pos:pos + NFFT] * np.exp(-2j * np.pi * cfo * np.arange(pos, pos + NFFT) / rate)
        third = y[sss_pos:sss_pos + NFFT] * np.exp(-2j * np.pi * cfo * np.arange(sss_pos, sss_pos + NFFT) / rate)
        yp, ys = np.fft.fft(first)[BINS], np.fft.fft(third)[BINS]
        equalized = ys * np.conj(yp) * pss(nid2)
        sss_scores = np.abs(catalog @ equalized) ** 2 / max(127 * np.vdot(equalized, equalized).real, 1e-30)
        order = np.argsort(sss_scores)
        identity, runner = int(order[-1]), int(order[-2])
        score, runner_score = float(sss_scores[identity]), float(sss_scores[runner])
        accepted = score >= 0.35 and score >= 3 * runner_score
        results.append(dict(analysis_sample=int(pos), offset_seconds=float(pos / rate),
                            pss_score=float(scores[pos]), nid2=nid2,
                            cfo_hz=cfo, pci=3 * identity + nid2, sss_score=score,
                            sss_runner_up_score=runner_score, sequence_evidence=accepted))
    return dict(max_pss_score=peak_score, nid2=nid2, coarse_cfo_hz=coarse, observations=results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--ssb-center-hz', type=float, required=True)
    parser.add_argument('--channel', type=int, choices=range(4), required=True)
    parser.add_argument('--milliseconds', type=float, default=40)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    info = json.loads((args.directory / 'capture_info.json').read_text())
    fs = info['sample_rate_hz']
    decimation = round(fs / 7680000)
    if decimation < 1 or fs != decimation * 7680000:
        raise ValueError('unsupported sample rate')
    count = round(args.milliseconds * fs / 1000)
    if count > info['samples_per_channel'] or count <= 0:
        raise ValueError('invalid analysis extent')
    raw = np.memmap(args.directory / f'rx{args.channel}.sc16', mode='r', dtype='<i2').reshape(-1, 2)
    y = raw[:count, 0].astype(np.float32) + 1j * raw[:count, 1].astype(np.float32)
    center = info['rf_channels'][args.channel]['frequency_hz']
    y *= np.exp(-2j * np.pi * (args.ssb_center_hz - center) * np.arange(count) / fs).astype(np.complex64)
    y = signal.resample_poly(y, 1, decimation)
    result = detect(y, 7680000)
    good = [x for x in result['observations'] if x['sequence_evidence']]
    consistent = len(good) >= 2 and len({x['pci'] for x in good}) == 1
    result.update(status='REPEATED_PSS_SSS_EVIDENCE' if consistent else 'UNRESOLVED',
                  scope='reference-frequency sequence coverage, not PBCH/MIB CRC or autonomous acquisition',
                  reference_ssb_center_hz=args.ssb_center_hz, channel=args.channel,
                  analyzed_milliseconds=args.milliseconds,
                  observed_intervals_ms=[1000 * (b['offset_seconds'] - a['offset_seconds']) for a, b in zip(good, good[1:])],
                  pbch_crc='NOT_TESTED', csi_rs='UNRESOLVED')
    with args.output.open('x') as dst:
        json.dump(result, dst, indent=2)
        dst.write('\n')
    print(json.dumps(result))


if __name__ == '__main__':
    main()
