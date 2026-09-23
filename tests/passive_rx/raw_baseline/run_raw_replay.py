#!/usr/bin/env python3
"""Run the normal passive receiver on validated raw IQ, never on an RF device.
Bootstrap grids are search hypotheses derived from capture geometry and the
repository's standards tables. They are NOT decoded cell/BWP configuration.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
from offline_cell_config import BW_MHZ, receiver_config, ss_ref, standard_tables
from validate_streamed_raw import validate


def bootstrap_hypotheses(info, rows, bandwidths, cfo_bound):
    fs = info['sample_rate_hz']
    centers = {c['frequency_hz'] for c in info['rf_channels']}
    if len(centers) != 1:
        raise ValueError('UNSUPPORTED: independently tuned capture channels')
    center = centers.pop()
    result = {}
    for band, mu, first, step, last in rows:
        if band >= 254 or mu not in (0, 1):
            continue
        rb = max(n for n, mhz in zip(bandwidths[mu], BW_MHZ) if n > 0 and mhz <= 100)
        fft = 1 << (12 * rb - 1).bit_length()
        scs = 15000 << mu
        if fft * scs != fs:
            continue
        reach = min(fs / 2, rb * 6 * scs) - 120 * scs - cfo_bound
        points = tuple(g for g in range(first, last + 1, step)
                       if abs(ss_ref(g) - center) <= reach)
        if not points:
            continue
        key = (mu, rb, points)
        if key not in result:
            result[key] = dict(mu=mu, prb=rb, fft=fft, center_hz=center,
                               possible_bands=[], gscn_candidates=list(points),
                               status='BOOTSTRAP_HYPOTHESIS_NOT_CELL_CONFIGURATION')
        result[key]['possible_bands'].append(band)
    return list(result.values())


def gate5_verdict(text, gap_armed):
    """Reacquisition (acceptance gate 5), judged from the receiver's own log.

    Only meaningful when a discontinuity was deliberately injected: the gap is the stimulus, the
    receiver's RXDISCONT is the detection, and the acquisition tracker leaving and re-entering a
    configured state is the recovery. Absent an injected gap there is nothing to judge, so this
    reports NOT_APPLICABLE rather than inventing a pass.
    """
    if not gap_armed:
        return {'verdict': 'NOT_APPLICABLE', 'reason': 'no discontinuity injected'}
    injected = 'RAW_IQ_GAP injected' in text
    detected = 'RXDISCONT' in text and 'INVALIDATING SYNC' in text
    states = re.findall(r'ACQ_STATE (\w+) -> (\w+)', text)
    lost_at = next((i for i, (_, to) in enumerate(states) if to == 'LOST'), None)
    recovered = lost_at is not None and any(
        to not in ('LOST', 'SEARCHING') for _, to in states[lost_at + 1:])
    final = states[-1][1] if states else None
    ok = injected and detected and lost_at is not None and recovered
    return {'verdict': 'PASS' if ok else 'FAIL',
            'gap_injected': injected, 'discontinuity_detected': detected,
            'acq_declared_lost': lost_at is not None, 'acq_recovered': recovered,
            'final_state': final, 'transitions': [f'{a}->{b}' for a, b in states]}


def parse_log(path, gap_armed=False):
    broadcasts = {'ssb': [], 'sib1': []}
    ready = eof = False
    faults = []
    for line in path.read_text(errors='replace').splitlines():
        ready |= 'RAW_IQ_READY ' in line
        eof |= 'RAW_IQ_EOF ' in line
        if re.search(r'RAW_IQ_VOID|RAW_IQ_UNSUPPORTED|Assertion \(|RFSTALL|RFTSDISC|RXDISCONT|Segmentation fault|unknown option:', line):
            # A deliberately injected gap makes RXDISCONT the STIMULUS, not a fault. Every other
            # fault signature still counts, and an un-armed run still treats RXDISCONT as a fault.
            if not (gap_armed and 'RXDISCONT' in line):
                faults.append(line.strip()[:1000])
        for name, marker in (('ssb', 'ISAC_ACQ_SSB '), ('sib1', 'ISAC_ACQ_SIB1 ')):
            if marker in line:
                try:
                    broadcasts[name].append(json.JSONDecoder().raw_decode(line.split(marker, 1)[1])[0])
                except ValueError:
                    faults.append('Malformed received-broadcast evidence: ' + line.strip()[:500])
    return dict(raw_backend_ready=ready, eof=eof, faults=faults, received_broadcasts=broadcasts)


def write_json(path, data):
    with path.open('x') as file:
        json.dump(data, file, indent=2)
        file.write('\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--hypothesis-index', type=int, default=0)
    p.add_argument('--speed', type=float, default=0.1, help='Sample pacing relative to real time, not signal resampling')
    p.add_argument('--timeout', type=float, default=180)
    p.add_argument('--cfo-bound-hz', type=int, default=200000)
    p.add_argument('--sample-shift', type=int, choices=range(16), required=True,
                   help='Explicit analysis amplitude scaling; original IQ is never modified')
    p.add_argument('--binary', type=Path, default=REPO / 'cmake_targets/ran_build/build/nr-uesoftmodem')
    p.add_argument('--gap-at-s', type=float, default=0, help='Inject one IQ discontinuity this many seconds into the recording (reacquisition test)')
    p.add_argument('--gap-s', type=float, default=0, help='Length of the injected discontinuity in seconds')
    args = p.parse_args()
    if not math.isfinite(args.speed) or not 0 < args.speed <= 1:
        p.error('speed must be finite and in (0, 1]')
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not 0 < args.cfo_bound_hz <= 480000:
        p.error('invalid timeout or CFO search bound')
    capture = args.capture.resolve(strict=True)
    info = validate(capture)
    rows, bandwidths = standard_tables()
    hypotheses = bootstrap_hypotheses(info, rows, bandwidths, args.cfo_bound_hz)
    if not 0 <= args.hypothesis_index < len(hypotheses):
        p.error('UNSUPPORTED: no selected exact-rate bootstrap grid; resampling is not implemented')
    trial = hypotheses[args.hypothesis_index]
    binary = args.binary.resolve(strict=True)
    library = (REPO / 'cmake_targets/ran_build/build/libraw_iq.so').resolve(strict=True)
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = out / 'receiver.conf'
    with config.open('x') as f:
        f.write(receiver_config(out, False))
        f.write('\ndevice = { name = "raw_iq"; };\n')
        f.write('loader = { raw_iq = { shlibpath = ' + json.dumps(str(library.parent)) + '; }; };\n')
    env = dict(PATH='/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
               HOME=str(Path.home()), LANG='C',
               LD_LIBRARY_PATH=f'{binary.parent}:/usr/local/lib',
               ISAC_RAW_IQ_DIRECTORY=str(capture), ISAC_RAW_IQ_SAMPLES=str(info['samples_per_channel']),
               ISAC_RAW_IQ_FIRST_TICK=str(info['first_sample_tick']), ISAC_RAW_IQ_CHANNELS=str(info['channels']),
               ISAC_RAW_IQ_RATE=str(info['sample_rate_hz']), ISAC_RAW_IQ_CENTER=str(round(trial['center_hz'])),
               ISAC_RAW_IQ_SPEED=str(args.speed), ISAC_RAW_IQ_SHIFT=str(args.sample_shift),
               ISAC_AUTO_ACQUIRE='1', ISAC_ACQ_PROBE='0', ISAC_ACQ_CFO_MAX_HZ=str(args.cfo_bound_hz),
               ISAC_SYNC_ONLY='1', ISAC_RX_MRC_MODE='2', ISAC_UL_RX_BRANCH='-1',
               ISAC_DMRS_FO_APPLY='0', ISAC_SFO_CORRECT='0', ISAC_RX_BRANCH_FO='0',
               ISAC_RX_GAIN_TRIM='0,0,0,0', ISAC_DISC_NO_RESYNC='0', ISAC_RF_STALL_MAX_REINIT='0',
               ISAC_CFO_TRACK_HZ='1', ISAC_CFO_TRACK_PERIOD='20', ISAC_PDCCH_TIMING='1',
               ISAC_PUSCH_TIMING='1', ISAC_PUSCH_DIAG='1', ISAC_UL_TA_SWEEP='0:0:0',
               ISAC_SENSE_COMB='0', ISAC_TSYNC_RESET='0')
    if args.gap_at_s > 0 and args.gap_s > 0:
        env.update(ISAC_RAW_IQ_GAP_AT_S=str(args.gap_at_s), ISAC_RAW_IQ_GAP_S=str(args.gap_s))
    # An explicit file-only module and a separate network namespace are mandatory.
    # sudo is used for the receiver's existing real-time priorities, not RF access.
    command = ['sudo', '-n', 'unshare', '--net', '--', 'env', '-i']
    command += [f'{k}={v}' for k, v in env.items()]
    command += [str(binary), '-O', str(config), '--passive-rx',
                '-r', str(trial['prb']), '--numerology', str(trial['mu']),
                '--band', str(trial['possible_bands'][0]), '-C', str(round(trial['center_hz'])),
                '--ue-nb-ant-rx', str(info['channels']), '--ue-nb-ant-tx', str(info['channels']),
                '--ue-rxgain', str(info['rf_channels'][0]['gain_db']), '--thread-pool', '-1',
                '--ue-scan-carrier', '--ue-fo-compensation', '--cont-fo-comp', '1',
                '--freq-sync-P', '.05', '--freq-sync-I', '.001', '--initial-fo', '0',
                '--ntn-initial-time-drift', '0', '--time-sync-I', '.01', '-A', '90']
    write_json(out / 'invocation.json', dict(command=command, env=env, capture=str(capture),
               capture_integrity='PASS', bootstrap_hypotheses=hypotheses,
               selected_bootstrap_index=args.hypothesis_index, cell_configuration_injected=False,
               network_namespace='ISOLATED', hardware_access='FORBIDDEN',
               limitation='Exact-rate 15/30-kHz bootstrap grids only; selected band is a search context, not a cell fact'))
    print(f'OFFLINE_REPLAY={out}', flush=True)
    start = time.monotonic()
    timed_out = False
    with (out / 'receiver.log').open('x') as log:
        process = subprocess.Popen(command, cwd=out, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            rc = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(process.pid, signal.SIGTERM)
            try:
                rc = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                rc = process.wait()
    gap_armed = args.gap_at_s > 0 and args.gap_s > 0
    result = parse_log(out / 'receiver.log', gap_armed)
    valid = rc == 0 and result['raw_backend_ready'] and result['eof'] and not result['faults'] and not timed_out
    gate5 = gate5_verdict((out / 'receiver.log').read_text(errors='replace'), gap_armed)
    result.update(status=('VALID_FAULT_INJECTION' if gap_armed else 'VALID_TRANSPORT_REPLAY') if valid else 'VOID',
                  receiver_returncode=rc, timed_out=timed_out, wall_seconds=time.monotonic()-start,
                  acceptance_gate_4='NOT_PASSED', acceptance_gate_5=gate5,
                  injected_gap={'at_s': args.gap_at_s, 'len_s': args.gap_s} if gap_armed else None,
                  note='EOF transport validity is not proof of acquisition, DCI interpretation, or PDSCH success')
    write_json(out / 'result.json', result)
    print(json.dumps(result), flush=True)
    return 0 if valid else 2


if __name__ == '__main__':
    sys.exit(main())
