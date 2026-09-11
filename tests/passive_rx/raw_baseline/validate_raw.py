#!/usr/bin/env python3
"""Device-free extent/timestamp/hash validation, not a claim of NR decode coverage."""
import argparse
import hashlib
import json
from pathlib import Path


def validate(root):
    info = json.loads((root / 'capture_info.json').read_text())
    if info['status'] != 'COMPLETE_CONTIGUOUS_RAW_IQ' or info['format'] != 'sc16_le':
        raise ValueError('incomplete/unsupported recording')
    count = info['samples_per_channel']
    first = info['first_sample_tick']
    if count <= 0 or info['channels'] != 4 or info['sample_rate_hz'] <= 0:
        raise ValueError('invalid sample geometry')
    offset = blocks = 0
    with (root / 'timestamps.tsv').open() as src:
        if src.readline().strip() != 'offset_samples\tcount_samples\tfirst_sample_tick':
            raise ValueError('unknown timestamp columns')
        for line in src:
            position, length, tick = map(int, line.split())
            if position != offset or length <= 0 or tick != first + offset:
                raise ValueError(f'timestamp discontinuity at sample {offset}')
            offset += length
            blocks += 1
    if offset != count or blocks != info['blocks']:
        raise ValueError('incomplete timestamp coverage')
    if info['rx_metadata_errors'] or info['timestamp_discontinuities']:
        raise ValueError('capture reported RF metadata failure')
    for channel in range(4):
        if (root / f'rx{channel}.sc16').stat().st_size != count * 4:
            raise ValueError(f'channel {channel} has wrong byte count')
    before, after = [(root / p).read_text().strip() for p in ('nic_missed_before.txt', 'nic_missed_after.txt')]
    if before != after:
        raise ValueError('NIC missed counter increased')
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    root = args.directory
    info = validate(root)
    checksums = {}
    for name in ['capture_info.json', 'timestamps.tsv', 'capture.log', 'capture_raw.cpp',
                 'capture_raw', 'validate_raw.py'] + [f'rx{c}.sc16' for c in range(4)]:
        digest = hashlib.sha256()
        with (root / name).open('rb') as src:
            for chunk in iter(lambda: src.read(4 * 1024 * 1024), b''):
                digest.update(chunk)
        checksums[name] = digest.hexdigest()
    report = {'status': 'VALID_CONTIGUOUS_RAW_IQ', 'duration_s': info['duration_s'],
              'channels': 4, 'sample_rate_hz': info['sample_rate_hz'],
              'samples_per_channel': info['samples_per_channel'],
              'hardware_timestamp_continuity': 'PASS', 'channel_file_extents': 'PASS',
              'nic_missed_counter_unchanged': True, 'nr_message_decode_coverage': 'NOT_YET_VALIDATED',
              'sha256': checksums}
    with (root / 'validation.json').open('x') as dst:
        json.dump(report, dst, indent=2)
        dst.write('\n')
    with (root / 'SHA256SUMS').open('x') as dst:
        for name, digest in checksums.items():
            dst.write(f'{digest}  {name}\n')
    print(json.dumps({k: v for k, v in report.items() if k != 'sha256'}))


if __name__ == '__main__':
    main()
